#include "inject/actions.h"

#include "inject/guard.h"
#include "inject/leased_call.h"
#include "inject/payload_status.h"
#include "inject/pe_export.h"
#include "inject/target.h"
#include "platform/log.h"
#include "platform/win32_util.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace deskfeed::inject {
namespace {

// 一次 `DeskFeedStop` 允许等待的毫秒数。负载内部对非法/0 值也用 10 秒兜底，两处一致。
// 注意：这是**负载自己**的收尾预算，不是注入器放弃等待的期限——线程没回来时一律 park。
constexpr unsigned kStopTimeoutMs = 10000;
// parking 的 tick：只是周期性让出时间片/打日志的粒度，**不是**放弃等待的期限。
constexpr DWORD kParkTickMs = 200;
// watch 两轮之间的退避。
constexpr unsigned long long kWatchRetryMs = 30000;

bool file_exists(std::wstring const& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 只有导出名与状态名这两处 ASCII 会被转成宽字符打日志，不为此拉进 UTF-8 转换层。
std::wstring ascii_to_wide(char const* text) {
    std::wstring out;
    for (; text != nullptr && *text != '\0'; ++text) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text)));
    }
    return out;
}

std::wstring directory_of(std::wstring const& path) {
    std::wstring::size_type const slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

void report(std::wstring const& text) { deskfeed::log_line(text); }

// ---- watch 的停止事件与"同一问题只报一次" --------------------------------------

HANDLE g_watch_stop = nullptr;
std::atomic<bool> g_watch_busy{false};

BOOL WINAPI watch_console_handler(DWORD type) {
    if (g_watch_stop != nullptr) {
        ::SetEvent(g_watch_stop);
    }
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        for (int i = 0; i < 40 && g_watch_busy.load(); ++i) {
            ::Sleep(100);
        }
    }
    return TRUE;
}

void report_problem(WatchState& state, std::wstring const& key, std::wstring const& text) {
    if (state.problem == key) {
        return;
    }
    state.problem = key;
    report(text);
}

// 一次受租约保护的远程调用为什么没有拿到实际结论——如实写进日志，绝不粉饰成"已清理"。
std::wstring describe_leased_call(LeaseCall const& call, wchar_t const* what) {
    std::wstring text = what;
    switch (call.outcome) {
        case LeaseCallOutcome::Completed:
            text += L" completed";
            break;
        case LeaseCallOutcome::NeverExecuted:
            text += L" was never called (the remote thread was never created)";
            break;
        case LeaseCallOutcome::TargetDied:
            text += L": the target process died while the entry thread was still running; the module "
                    L"went away with it, which is not a clean unload";
            break;
        case LeaseCallOutcome::Interrupted:
            text += L": the wait was interrupted; the entry thread has no conclusion yet";
            break;
        case LeaseCallOutcome::Error:
            text += L": the call has no conclusion";
            break;
    }
    if (!call.error.empty()) {
        text += L" (" + call.error + L")";
    }
    return text;
}

// 一次动作的 actor：**一次动作只捕获一个**目标进程句柄，并且所有远程调用都在它上面做。
struct Actor {
    TargetProcess target;
    GuardLease lease;
    HANDLE interrupt_event = nullptr;

    // 五个调用点（LoadLibraryW / Start / GetStatus / Stop / FreeLibrary）统一的写序 + parking。
    LeaseCall call(std::function<RemoteCall()> const& issue) {
        return run_leased_call(lease, target, kParkTickMs, issue, interrupt_event);
    }

    // 破坏性动作之前再核对一次：**捕获的**那个进程还在不在。绝不按 PID 重开一个新进程继续动作。
    bool require_alive(std::wstring& why) const {
        if (target_process_alive(target)) {
            return true;
        }
        why = L"the captured target process (pid " + std::to_wstring(target.process_id) +
              L") is gone; the action ends here and is never retried against a freshly opened process";
        return false;
    }
};

struct ActorResult {
    int code = 1;
    bool ran = false;            // body 是否真的跑过（拿不到租约时为 false）
    bool ended_unknown = false;  // 本 actor 以未知收尾：本线程必须结束，不得再 acquire
};

using ActorBody = std::function<ActorResult(Actor&)>;

// 动作的 actor 外壳：解析出来的目标 → 捕获句柄（只此一次）→ 拿租约 → 跑 body → 按**事实**收尾。
//
// 收尾只有两条路：
//   * 没有任何未决远程调用 → 唯一的释放点 `conclude_clean()`；
//   * 有未决调用（目标死亡 / 打断 / 系统调用失败）→ `abandon_unknown()`：置粘性事件、**不**释放、
//     **不**关闭三份事实句柄，并告诉调用方本线程必须结束。
ActorResult run_actor(ActionContext const& context, HANDLE interrupt_event, ActorBody const& body) {
    ActorResult result;
    Actor actor;
    std::wstring error;
    if (!open_target_process(context.process_id, context.creation_time, actor.target, error)) {
        report(L"cannot capture the target process (pid " + std::to_wstring(context.process_id) +
               L"): " + error);
        return result;
    }

    GuardTarget const guard_target{actor.target.process_id, actor.target.creation_time};
    GuardAcquire const acquired = actor.lease.acquire(guard_target, error);
    if (!guard_acquire_allows_action(acquired)) {
        switch (acquired) {
            case GuardAcquire::Busy:
                report(L"another actor holds the payload guard lease for pid " +
                       std::to_wstring(actor.target.process_id) +
                       L"; doing nothing (no queueing, no takeover)");
                break;
            case GuardAcquire::Unknown:
                report(L"the payload guard for pid " + std::to_wstring(actor.target.process_id) +
                       L" is uncertain (" + error +
                       L"); refusing to touch the target (no Stop/Start/eject)");
                break;
            default:
                report(L"cannot acquire the payload guard for pid " +
                       std::to_wstring(actor.target.process_id) + L": " + error);
                break;
        }
        close_target_process(actor.target);
        return result;
    }

    if (!target_process_alive(actor.target)) {
        // 捕获的进程已经没了：绝不用 fresh PID 重开继续动作。留下未知事实并让本线程结束。
        report(L"the captured target process (pid " + std::to_wstring(actor.target.process_id) +
               L") is already gone; the action ends without touching anything");
        actor.lease.abandon_unknown();
        result.ended_unknown = true;
        close_target_process(actor.target);
        return result;
    }

    actor.interrupt_event = interrupt_event;
    ActorResult const body_result = body(actor);
    result.code = body_result.code;
    result.ran = true;
    result.ended_unknown = body_result.ended_unknown;

    if (actor.lease.has_pending_remote_call()) {
        actor.lease.abandon_unknown();
        result.ended_unknown = true;
        report(L"note: an entry thread did not reach a real conclusion; the lease stays unreleased, "
               L"the sticky uncertainty stays set, and this thread must end");
    } else {
        actor.lease.note_action_conclusive(true);
        std::wstring conclude_error;
        if (!actor.lease.conclude_clean(conclude_error)) {
            actor.lease.abandon_unknown();
            result.ended_unknown = true;
            report(L"note: the lease could not be released cleanly (" + conclude_error +
                   L"); keeping the facts and ending this thread");
        }
    }
    close_target_process(actor.target);
    return result;
}

// ---------------------------------------------------------------------------
// 四个动作共用的两段
// ---------------------------------------------------------------------------

// 卸载一个目标：远程 `DeskFeedStop` → 只有报"可以卸载"才远程 `FreeLibrary` → 模块表确认真的不在。
// **必须**在属于该目标的 actor 里调用（租约持有中）。返回 false 时模块仍留在目标进程里。
bool stop_one(Actor& actor, std::wstring const& dll_path, bool no_unload, bool& retired,
              std::wstring& error);

bool eject_one(Actor& actor, unsigned long long module_base, bool& retired, std::wstring& error) {
    retired = false;
    // `FreeLibrary` 是**另一次**远程调用，单独记一次写序（PendingNoTid → 真实 TID → 真实 signal
    // → Idle）。只有它真的退出之后才谈"模块是不是真的不在表里"。
    std::wstring call_error;
    LeaseCall const free_call = actor.call([&]() {
        return issue_free_library_on(actor.target.process, actor.target.process_id, module_base,
                                     call_error);
    });
    if (!free_call.completed()) {
        error = describe_leased_call(free_call, L"FreeLibrary") + L"; keeping the module mapped";
        return false;
    }
    if (free_call.call.exit_code == 0) {
        error = L"FreeLibrary returned FALSE in the target process";
        return false;
    }
    if (!confirm_module_gone(actor.target.process_id, module_base, error)) {
        return false;
    }
    retired = true;
    return true;
}

bool stop_one(Actor& actor, std::wstring const& dll_path, bool no_unload, bool& retired,
              std::wstring& error) {
    retired = false;
    unsigned long const pid = actor.target.process_id;

    std::wstring gone;
    if (!actor.require_alive(gone)) {
        error = gone;
        return false;
    }

    ModuleEntry module;
    std::wstring lookup_error;
    switch (lookup_module_by_full_path(pid, dll_path, module, lookup_error)) {
        case ModuleLookup::Found:
            break;
        case ModuleLookup::NotFound:
            report(L"payload is not loaded in pid " + std::to_wstring(pid) + L" (" + lookup_error +
                   L")");
            retired = true;
            return true;
        case ModuleLookup::Error:
            error = L"cannot read the module list of pid " + std::to_wstring(pid) + L": " +
                    lookup_error;
            return false;
    }
    report(L"payload loaded in pid " + std::to_wstring(pid) + L": base=" +
           deskfeed::hex_text(module.base) + L" size=" + deskfeed::hex_text(module.size));

    PeExportError kind = PeExportError::None;
    PeExportInfo info;
    std::wstring export_error;
    if (!pe_export_rva(dll_path, kStopExportName, info, export_error, &kind)) {
        error = export_error;
        return false;
    }
    // 磁盘上那份镜像必须与目标进程里装载的那份是同一个版本：否则 RVA 加到一个不同 build 的
    // 基址上就是跳到别的代码。SizeOfImage 只是快速前置，之后还要读远程镜像自己的 PE 元数据。
    if (!pe_image_matches_module(info, module.size, error)) {
        return false;
    }
    unsigned long verified_rva = 0;
    if (!verify_remote_export(pid, module.base, module.size, kStopExportName, info, verified_rva,
                              error)) {
        error = L"the loaded payload does not match the on-disk DLL: " + error;
        return false;
    }

    // 超时值按 uintptr_t 编码在参数指针本身里（不解引用），这是冻结的负载 ABI。
    void* const encoded_timeout = reinterpret_cast<void*>(static_cast<std::uintptr_t>(kStopTimeoutMs));
    LeaseCall const stop_call = actor.call([&]() {
        return issue_remote_thread_on(actor.target.process,
                                      reinterpret_cast<void*>(module.base + verified_rva),
                                      encoded_timeout);
    });
    if (!stop_call.completed()) {
        error = describe_leased_call(stop_call, L"DeskFeedStop") + L"; keeping the module mapped";
        return false;
    }

    UnloadDecision const decision =
        unload_decision(stop_call.call.exit_code, stop_call.call.signaled, no_unload);
    if (decision == UnloadDecision::KeepLoaded) {
        error = L"DeskFeedStop reported that the payload is not retired (exit code " +
                deskfeed::hex_text(stop_call.call.exit_code) + L"); keeping the module mapped";
        return false;
    }
    if (decision == UnloadDecision::KeepNoUnload) {
        report(L"the payload acknowledged the stop; leaving the DLL mapped (--no-unload)");
        retired = true;
        return true;
    }

    report(L"the payload retired; unloading the module");
    return eject_one(actor, module.base, retired, error);
}

// 别的 explorer.exe 里可能还留着一份负载（以前装错过进程，或者旧 explorer 还没退）。留着它
// 会一直占着 DLL、也会在下一次构建时报 LNK1168，而且它自己那侧永远插不出读数块。
struct CleanOthers {
    bool ok = true;             // false = 至少有一份没能安全撤下
    bool ended_unknown = false; // true = 出现未决事实：本线程必须结束
};

CleanOthers clean_other_explorers(ActionContext const& owner, std::wstring const& dll_path,
                                  bool dry_run) {
    CleanOthers result;
    std::vector<ProcessEntry> others;
    std::wstring error;
    if (!list_processes_by_image(owner.other_image_name, others, error)) {
        report(L"cannot enumerate " + owner.other_image_name + L" processes: " + error);
        return result;  // 枚举失败不算"有残留没清掉"
    }

    for (ProcessEntry const& other : others) {
        if (other.process_id == owner.process_id) {
            continue;
        }
        ModuleEntry stale;
        std::wstring stale_error;
        switch (lookup_module_by_full_path(other.process_id, dll_path, stale, stale_error)) {
            case ModuleLookup::NotFound:
                continue;
            case ModuleLookup::Error:
                report(L"cannot inspect pid " + std::to_wstring(other.process_id) +
                       L" for a stale payload: " + stale_error);
                continue;
            case ModuleLookup::Found:
                break;
        }
        report(L"another " + owner.other_image_name + L" (pid " + std::to_wstring(other.process_id) +
               L") also has the payload loaded at " + deskfeed::hex_text(stale.base));
        if (dry_run) {
            report(L"would stop and unload it");
            continue;
        }

        // 别的 PID 是**另一个目标**：它要自己捕获的句柄与自己的租约，绝不拿当前这份租约替它。
        ActionContext other_context;
        other_context.process_id = other.process_id;
        other_context.creation_time = other.creation_time;
        other_context.image_path = other.image_path;
        other_context.other_image_name = owner.other_image_name;

        ActorResult const stopped = run_actor(other_context, nullptr, [&](Actor& victim) {
            ActorResult out;
            bool retired = false;
            std::wstring stop_error;
            if (!stop_one(victim, dll_path, /*no_unload=*/false, retired, stop_error)) {
                report(L"could not stop the payload in pid " +
                       std::to_wstring(victim.target.process_id) + L": " + stop_error);
                out.code = 1;
                return out;
            }
            out.code = 0;
            return out;
        });
        if (stopped.ended_unknown) {
            result.ok = false;
            result.ended_unknown = true;
            return result;
        }
        if (stopped.code != 0) {
            result.ok = false;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// load / stop / status / watch 的 body
// ---------------------------------------------------------------------------

ActorResult load_body(Actor& actor, ActionContext const& context, Options const& options,
                      std::wstring const& dll_path) {
    ActorResult out;
    unsigned long const pid = actor.target.process_id;

    // 位数的硬门禁：x64 负载装不进 32 位（WOW64）任务栏属主，读不到架构也拒绝。
    std::wstring arch_error;
    if (!target_is_native_x64(pid, arch_error)) {
        report(arch_error);
        out.code = 1;
        return out;
    }

    PayloadStatus status;
    std::wstring status_error;
    if (!query_payload_status(actor.lease, actor.target, dll_path, options.scope, status,
                              status_error)) {
        report(L"cannot query the payload state: " + status_error);
        out.code = 1;
        return out;
    }
    report(std::wstring(L"current payload state: ") +
           ascii_to_wide(payload_state_name(status.state)) + L" (" + status.detail + L")");

    // 目标这一份动或不动，别的实例里的残留每次都要按同一套安全门清一遍。
    // 返回 false 表示"本线程必须结束"（出现未决事实），调用方立刻停下。
    auto clean_others = [&]() -> bool {
        if (options.dry_run) {
            report(L"dry run: would still retire stale payloads in other " +
                   context.other_image_name + L" processes");
            return true;
        }
        CleanOthers const cleaned = clean_other_explorers(context, dll_path, /*dry_run=*/false);
        if (cleaned.ended_unknown) {
            out.ended_unknown = true;
            return false;
        }
        if (!cleaned.ok) {
            report(L"stale-other-retained: a stale payload in another " +
                   context.other_image_name +
                   L" could not be safely removed (deployment requires no stale payload elsewhere)");
        }
        return true;
    };

    if (status.mapped) {
        if (status.state == PayloadState::MappedRetained) {
            report(L"refusing to reload: the mapped payload cannot prove it is retired (" +
                   status.detail + L"); fix or stop it first");
            if (!clean_others()) {
                return out;
            }
            out.code = 1;
            return out;
        }
        if (status.state == PayloadState::MappedActive) {
            report(L"the payload is already running on this scope and bound to the live producer; "
                   L"nothing to do for this target");
            if (!clean_others()) {
                return out;
            }
            report(L"nothing was injected for this target");
            out.code = 0;
            return out;
        }
        if (status.state == PayloadState::MappedUnknownAbi ||
            status.state == PayloadState::MappedUnverifiable) {
            // 结论不可信（没有状态 ABI / 远程调用没有实际结论 / POD 不可信 / 身份不一致）：
            // **绝不**去 Stop 或卸载。这种模块里可能正有一个没退出的远程线程在跑它的代码。
            report(L"refusing to reload: the payload state is not verifiable (" +
                   ascii_to_wide(payload_state_name(status.state)) + L": " + status.detail +
                   L"); not stopping anything in this run");
            if (!clean_others()) {
                return out;
            }
            out.code = 1;
            return out;
        }
        if (options.dry_run) {
            report(L"would stop the mapped payload (it is not provably active) and load it again");
            report(L"dry run: would also retire stale payloads in other " + context.other_image_name +
                   L" processes");
            out.code = 0;
            return out;
        }
        if (!state_allows_reload_stop(status)) {
            // 兜底：任何不在"可信 + Inactive"这一格的组合都不允许先停再装。
            report(std::wstring(L"refusing to reload: the payload state '") +
                   ascii_to_wide(payload_state_name(status.state)) +
                   L"' does not allow a stop-then-load");
            out.code = 1;
            return out;
        }
        report(L"the mapped payload is not provably active; removing it before loading again");
        bool retired = false;
        std::wstring stop_error;
        if (!stop_one(actor, dll_path, /*no_unload=*/false, retired, stop_error)) {
            report(L"cannot remove the stale payload: " + stop_error);
            out.code = 1;
            return out;
        }
    }

    if (options.dry_run) {
        report(L"would inject " + dll_path + L" into pid " + std::to_wstring(pid) + L" and call " +
               ascii_to_wide(kStartExportName) + L" with scope \"" + options.scope + L"\"");
        report(L"dry run: nothing was injected");
        out.code = 0;
        return out;
    }

    if (!clean_others()) {
        return out;
    }

    std::wstring gone;
    if (!actor.require_alive(gone)) {
        report(gone);
        out.code = 1;
        return out;
    }

    // 已经装载（按完整路径命中）时不再装载一次：重复 `LoadLibraryW` 只会多加一次引用，让后面的
    // 卸载永远还不上。
    ModuleEntry existing;
    std::wstring lookup_error;
    switch (lookup_module_by_full_path(pid, dll_path, existing, lookup_error)) {
        case ModuleLookup::Found:
            report(L"the payload is already mapped in the target (another injector won the race); "
                   L"run '--status' to see whether it is active");
            out.code = 1;
            return out;
        case ModuleLookup::Error:
            report(L"cannot read the target module list: " + lookup_error);
            out.code = 1;
            return out;
        case ModuleLookup::NotFound:
            break;
    }

    // 第一步：远程 `LoadLibraryW`（它自己的写序周期）。
    RemoteBuffer path_buffer;
    bool path_allocated = false;
    std::wstring issue_error;
    LeaseCall const load_call = actor.call([&]() -> RemoteCall {
        return issue_load_library_on(actor.target.process, pid, dll_path, path_buffer,
                                     path_allocated, issue_error);
    });
    // 路径缓冲只在"线程从未创建"或"线程确实退出"之后释放；否则目标里的 LoadLibraryW 可能还在
    // 读那个路径串（释放它就是 use-after-free）。
    if (path_allocated &&
        may_release_remote_buffer(load_call.call.thread_created, load_call.call.signaled)) {
        remote_free_on(actor.target.process, path_buffer);
        path_allocated = false;
    }
    if (!load_call.completed()) {
        report(L"injection failed: " + describe_leased_call(load_call, L"LoadLibraryW"));
        out.code = 1;
        return out;
    }

    ModuleEntry loaded;
    std::wstring confirm_error;
    if (!confirm_load_library(pid, dll_path, load_call.call, loaded, confirm_error)) {
        report(L"injection failed: " + confirm_error);
        out.code = 1;
        return out;
    }
    report(L"payload loaded: base=" + deskfeed::hex_text(loaded.base) + L" size=" +
           deskfeed::hex_text(loaded.size) + L" path=" + loaded.path);

    PeExportError kind = PeExportError::None;
    PeExportInfo start_info;
    std::wstring export_error;
    if (!pe_export_rva(dll_path, kStartExportName, start_info, export_error, &kind)) {
        report(L"cannot locate " + ascii_to_wide(kStartExportName) + L": " + export_error);
        out.code = 1;
        return out;
    }
    if (!pe_image_matches_module(start_info, loaded.size, export_error)) {
        report(L"cannot use the on-disk export offsets: " + export_error);
        out.code = 1;
        return out;
    }
    // 尺寸相同不等于同一个 build：读一次远程镜像自己的 PE 元数据/导出表，用远程确认过的 RVA。
    unsigned long start_rva = 0;
    if (!verify_remote_export(pid, loaded.base, loaded.size, kStartExportName, start_info, start_rva,
                              export_error)) {
        report(L"the loaded payload does not match the on-disk DLL; refusing to call into it: " +
               export_error);
        out.code = 1;
        return out;
    }

    if (!actor.require_alive(gone)) {
        report(gone);
        out.code = 1;
        return out;
    }

    // 第二步：远程 `DeskFeedStart`（另一次写序周期）。
    RemoteBuffer scope_buffer;
    bool scope_allocated = false;
    LeaseCall const start_call = actor.call([&]() -> RemoteCall {
        if (!options.scope.empty()) {
            std::size_t const bytes = (options.scope.size() + 1) * sizeof(wchar_t);
            if (!remote_alloc_on(actor.target.process, bytes, scope_buffer, issue_error)) {
                RemoteCall failed;
                failed.error = issue_error;
                return failed;
            }
            scope_allocated = true;
            if (!remote_write_on(actor.target.process, scope_buffer.address, options.scope.c_str(),
                                 bytes, issue_error)) {
                RemoteCall failed;
                failed.error = issue_error;
                return failed;
            }
        }
        return issue_remote_thread_on(actor.target.process,
                                      reinterpret_cast<void*>(loaded.base + start_rva),
                                      scope_allocated
                                          ? reinterpret_cast<void*>(scope_buffer.address)
                                          : nullptr);
    });
    if (scope_allocated &&
        may_release_remote_buffer(start_call.call.thread_created, start_call.call.signaled)) {
        remote_free_on(actor.target.process, scope_buffer);
        scope_allocated = false;
    } else if (scope_allocated) {
        report(L"note: the remote scope buffer stays allocated because the Start thread has not "
               L"exited; it is released when the target process exits");
    }
    if (!start_call.completed()) {
        report(L"the Start call has no real conclusion: " +
               describe_leased_call(start_call, L"DeskFeedStart"));
        out.code = 1;
        return out;
    }

    switch (start_call.call.exit_code) {
        case 1:
            report(L"injection succeeded: the payload reported that it started");
            out.code = 0;
            return out;
        case 2:
            report(L"the payload rejected the request (bad scope pointer or a scope that is not on "
                   L"the whitelist)");
            out.code = 1;
            return out;
        case 3: {
            report(L"the payload reports that it is already running");
            PayloadStatus again;
            std::wstring again_error;
            bool const queried =
                query_payload_status(actor.lease, actor.target, dll_path, options.scope, again,
                                     again_error);
            if (queried && again.state == PayloadState::MappedActive) {
                report(L"and it is provably active on this scope; treating the load as satisfied");
                out.code = 0;
                return out;
            }
            std::wstring const detail = queried ? again.detail : again_error;
            report(L"but it cannot be proven active on this scope (" + detail +
                   L"); stopping it is the safe next step");
            out.code = 1;
            return out;
        }
        case 4:
            report(L"the payload could not start its worker thread");
            out.code = 1;
            return out;
        default:
            report(L"the payload's Start entry point returned an unknown code: " +
                   deskfeed::hex_text(start_call.call.exit_code));
            out.code = 1;
            return out;
    }
}

ActorResult stop_victim_body(Actor& actor, Options const& options, std::wstring const& dll_path) {
    ActorResult out;
    unsigned long const pid = actor.target.process_id;

    ModuleEntry module;
    std::wstring find_error;
    switch (lookup_module_by_full_path(pid, dll_path, module, find_error)) {
        case ModuleLookup::NotFound:
            report(L"payload is not loaded in pid " + std::to_wstring(pid) + L" (already removed)");
            out.code = 0;
            return out;
        case ModuleLookup::Error:
            report(L"cannot read the module list of pid " + std::to_wstring(pid) + L": " + find_error);
            out.code = 1;
            return out;
        case ModuleLookup::Found:
            break;
    }
    if (options.dry_run) {
        report(L"would stop the payload in pid " + std::to_wstring(pid) +
               (options.unload ? L" and unload the DLL" : L" and leave the DLL mapped"));
        out.code = 0;
        return out;
    }

    // 状态门禁：只有**结论可信**的目标才允许 Stop。目标死亡、未决调用、没有状态 ABI、POD 不可信、
    // 身份不一致一律跳过 —— 那种模块里可能正有一个没退出的远程线程在跑它的代码。
    PayloadStatus status;
    std::wstring status_error;
    bool const queried =
        query_payload_status(actor.lease, actor.target, dll_path, options.scope, status, status_error);
    if (!queried || !state_allows_stop(status)) {
        std::wstring const reason =
            queried ? ascii_to_wide(payload_state_name(status.state)) + L" (" + status.detail + L")"
                    : L"status query failed (" + status_error + L")";
        report(L"skipping pid " + std::to_wstring(pid) + L": " + reason +
               L"; refusing to stop an unverifiable payload");
        out.code = 1;
        return out;
    }

    bool retired = false;
    std::wstring stop_error;
    // `stop_one` 的参数是 no_unload，与用户的 `--no-unload` 同义；`options.unload` 是反过来的意图。
    // 这里曾经把两者直接传反：默认的 `--stop` 被当成 `--no-unload`，模块只停线程、不卸载。
    if (!stop_one(actor, dll_path, !options.unload, retired, stop_error)) {
        report(L"could not stop the payload in pid " + std::to_wstring(pid) + L": " + stop_error);
        out.code = 1;
        return out;
    }
    report(L"the payload in pid " + std::to_wstring(pid) +
           (options.unload ? L" was stopped and the DLL unloaded" : L" was stopped (DLL kept)"));
    out.code = 0;
    return out;
}

ActorResult watch_body(Actor& actor, ActionContext const& context, Options const& options,
                       WatchState& state, std::wstring const& dll_path) {
    ActorResult out;

    PayloadStatus status;
    std::wstring status_error;
    if (!query_payload_status(actor.lease, actor.target, dll_path, options.scope, status,
                              status_error)) {
        state.was_active = false;
        report_problem(state, L"query",
                             L"watch: cannot query the payload state: " + status_error);
        out.code = 1;
        return out;
    }

    if (status.state == PayloadState::MappedActive) {
        if (!state.was_active) {
            report(L"watch: the payload is active on this scope");
        }
        state.was_active = true;
        state.problem.clear();
        state.next_attempt_ms = 0;
        out.code = 0;
        return out;
    }

    if (status.state == PayloadState::MappedRetained) {
        state.was_active = false;
        report_problem(state, L"retained",
                             L"watch: the mapped payload is not retired (" + status.detail +
                                 L"); waiting instead of forcing an unload");
        out.code = 1;
        return out;
    }

    if (options.dry_run) {
        report_problem(state, L"dry",
                             L"watch: would load the payload (" + status.detail + L")");
        state.was_active = false;
        out.code = 0;
        return out;
    }

    if (state.next_attempt_ms != 0 && ::GetTickCount64() < state.next_attempt_ms) {
        out.code = 1;
        return out;
    }

    report_problem(state, L"load", L"watch: loading the payload (" + status.detail + L")");
    ActorResult const loaded = load_body(actor, context, options, dll_path);
    out.code = loaded.code;
    out.ended_unknown = loaded.ended_unknown;
    if (loaded.code != 0) {
        state.was_active = false;
        if (loaded.ended_unknown) {
            report_problem(state, L"load-failed",
                                 L"watch: the load ended with an unproven entry thread; the watch "
                                 L"stops so that this thread really ends");
        } else {
            state.next_attempt_ms = ::GetTickCount64() + kWatchRetryMs;
            report_problem(state, L"load-failed",
                                 L"watch: the load did not succeed; the next attempt is 30 s later");
        }
        return out;
    }
    state.was_active = true;
    state.problem.clear();
    state.next_attempt_ms = 0;
    return out;
}

}  // namespace

std::wstring payload_path_for(Options const& options) {
    if (!options.payload_path.empty()) {
        return options.payload_path;
    }
    return deskfeed::join_path(directory_of(deskfeed::module_self_path()), kPayloadFileName);
}

UnloadDecision unload_decision(DWORD stop_exit_code, bool thread_signaled, bool no_unload) {
    // 硬性门禁：没等到远程线程退出，就没有"已经卸载完"的证明——哪怕退出码看起来是"可以卸载"。
    if (!thread_signaled) {
        return UnloadDecision::KeepLoaded;
    }
    if (stop_exit_code != 1) {
        return UnloadDecision::KeepLoaded;
    }
    return no_unload ? UnloadDecision::KeepNoUnload : UnloadDecision::Unload;
}

namespace {

std::wstring target_description(ActionContext const& context) {
    std::wstring text = L"target " + context.other_image_name + L" pid=" +
                        std::to_wstring(context.process_id);
    if (!context.image_path.empty()) {
        text += L" image=" + context.image_path;
    }
    return text;
}

}  // namespace

bool resolve_action_target(ActionContext& context, std::wstring& error) {
    ProcessEntry owner;
    if (!resolve_taskbar_owner(owner, error)) {
        return false;
    }
    context.process_id = owner.process_id;
    context.creation_time = owner.creation_time;
    context.image_path = owner.image_path;
    return true;
}

int do_load_in(ActionContext const& context, Options const& options) {
    std::wstring const dll_path = payload_path_for(options);
    if (!file_exists(dll_path)) {
        report(L"payload DLL not found: " + dll_path);
        return 1;
    }
    report(L"payload: " + dll_path);
    report(target_description(context) + L" scope=\"" + options.scope + L"\"");

    ActorResult const result = run_actor(context, nullptr, [&](Actor& actor) {
        return load_body(actor, context, options, dll_path);
    });
    return result.code;
}

int do_load(Options const& options) {
    ActionContext context;
    std::wstring error;
    if (!resolve_action_target(context, error)) {
        report(error);
        return 1;
    }
    return do_load_in(context, options);
}

int do_stop_in(ActionContext const& context, Options const& options) {
    std::wstring const dll_path = payload_path_for(options);
    if (!file_exists(dll_path)) {
        report(L"payload DLL not found: " + dll_path);
        return 1;
    }
    report(target_description(context) + L" scope=\"" + options.scope + L"\"");

    // 目标 + 别的实例里的残留，一份都不留。先只读枚举，没装载的进程不会被碰（也不会为它建租约）。
    std::vector<ActionContext> victims;
    victims.push_back(context);

    std::vector<ProcessEntry> others;
    std::wstring list_error;
    if (list_processes_by_image(context.other_image_name, others, list_error)) {
        for (ProcessEntry const& other : others) {
            if (other.process_id == context.process_id) {
                continue;
            }
            ModuleEntry stale;
            std::wstring stale_error;
            switch (lookup_module_by_full_path(other.process_id, dll_path, stale, stale_error)) {
                case ModuleLookup::Found: {
                    report(L"another " + context.other_image_name + L" (pid " +
                           std::to_wstring(other.process_id) +
                           L") also has the payload loaded; stopping it too");
                    ActionContext extra;
                    extra.process_id = other.process_id;
                    extra.creation_time = other.creation_time;
                    extra.image_path = other.image_path;
                    extra.other_image_name = context.other_image_name;
                    victims.push_back(std::move(extra));
                    break;
                }
                case ModuleLookup::NotFound:
                    break;
                case ModuleLookup::Error:
                    report(L"cannot inspect pid " + std::to_wstring(other.process_id) + L": " +
                           stale_error);
                    break;
            }
        }
    } else {
        report(L"cannot enumerate " + context.other_image_name + L" processes: " + list_error);
    }

    bool all_ok = true;
    for (ActionContext const& victim : victims) {
        ActorResult const result = run_actor(victim, nullptr, [&](Actor& actor) {
            return stop_victim_body(actor, options, dll_path);
        });
        if (result.code != 0) {
            all_ok = false;
        }
        if (result.ended_unknown) {
            report(L"stop: a target ended without a proven conclusion; stopping here so that this "
                   L"thread really ends");
            return 1;
        }
    }

    if (options.dry_run) {
        report(L"dry run: nothing was changed");
    }
    return all_ok ? 0 : 1;
}

int do_stop(Options const& options) {
    ActionContext context;
    std::wstring error;
    if (!resolve_action_target(context, error)) {
        report(error);
        return 1;
    }
    return do_stop_in(context, options);
}

int do_status_in(ActionContext const& context, Options const& options) {
    std::wstring const dll_path = payload_path_for(options);
    ActorResult const result = run_actor(context, nullptr, [&](Actor& actor) -> ActorResult {
        ActorResult out;
        PayloadStatus status;
        std::wstring status_error;
        if (!query_payload_status(actor.lease, actor.target, dll_path, options.scope, status,
                                  status_error)) {
            report(L"status: cannot tell (" + status_error + L")");
            out.code = 2;
            return out;
        }

        report(L"status: " + ascii_to_wide(payload_state_name(status.state)) + L" pid=" +
               std::to_wstring(actor.target.process_id) + L" scope=\"" + options.scope + L"\"");
        if (!status.detail.empty()) {
            report(L"detail: " + status.detail);
        }
        if (status.mapped) {
            report(L"module: base=" + deskfeed::hex_text(status.module.base) + L" size=" +
                   deskfeed::hex_text(status.module.size));
        }
        if (status.have_pod) {
            report(L"payload: started=" + std::to_wstring(status.pod.started) + L" thread_alive=" +
                   std::to_wstring(status.pod.thread_alive) + L" mailbox_attached=" +
                   std::to_wstring(status.pod.mailbox_attached) + L" producer_alive=" +
                   std::to_wstring(status.pod.producer_alive) + L" block_present=" +
                   std::to_wstring(status.pod.xaml_block_present) + L" unload_safe=" +
                   std::to_wstring(status.pod.unload_safe) + L" dangling=" +
                   std::to_wstring(status.pod.dangling));
        }
        if (status.mailbox_present) {
            report(L"mailbox producer: pid=" + std::to_wstring(status.mailbox_producer_pid) +
                   L" alive=" + (status.mailbox_producer_alive ? L"yes" : L"no") + L" consistent=" +
                   (status.producer_consistent ? L"yes" : L"no"));
        }

        switch (status.state) {
            case PayloadState::MappedActive:
                out.code = 0;
                break;
            case PayloadState::MappedRetained:
                report(L"status: the payload cannot be safely reloaded; not loaded is NOT the answer "
                       L"here (exit 2)");
                out.code = 2;
                break;
            case PayloadState::NotMapped:
            case PayloadState::MappedInactive:
            case PayloadState::MappedUnknownAbi:
                out.code = 1;
                break;
            default:
                out.code = 2;
                break;
        }
        return out;
    });

    // 拿不到租约（另一位 actor 正在动作、或上一位留下未知）就是"问不出来"：绝不给 0。
    if (!result.ran) {
        return 2;
    }
    return result.code;
}

int do_status(Options const& options) {
    ActionContext context;
    std::wstring error;
    if (!resolve_action_target(context, error)) {
        report(error);
        report(L"status: cannot tell (no taskbar owner)");
        return 2;
    }
    return do_status_in(context, options);
}

int watch_round_in(ActionContext const& context, Options const& options, WatchState& state,
                   HANDLE interrupt_event) {
    std::wstring const dll_path = payload_path_for(options);

    if (state.pid != context.process_id) {
        if (state.pid != 0) {
            report(L"watch: the taskbar owner changed (pid " + std::to_wstring(state.pid) + L" -> " +
                   std::to_wstring(context.process_id) + L")");
        } else {
            report(L"watch: " + target_description(context));
        }
        state.pid = context.process_id;
        state.was_active = false;
        state.problem.clear();
        state.next_attempt_ms = 0;
    }

    // **每一轮各自拿一份租约**：一轮结束（无论干净还是未知）就丢弃这个 actor，绝不在同一条线程上
    // 带着上一次的租约进入下一轮。
    ActorResult const round = run_actor(context, interrupt_event, [&](Actor& actor) {
        return watch_body(actor, context, options, state, dll_path);
    });
    if (round.ended_unknown) {
        // 本轮留下了未证实的入口线程：**本线程必须真的结束**。同一条线程再 acquire 会按 Win32
        // 语义递归拿到同一把 mutex（`WAIT_OBJECT_0`，不是 `WAIT_ABANDONED`），从而把上一位留下的
        // 未知当成良性残留抹平——所以这里直接告诉调用方收工。
        return kWatchRoundLostUncertain;
    }
    return round.code;
}

int watch_round(Options const& options, WatchState& state) {
    ActionContext context;
    std::wstring error;
    if (!resolve_action_target(context, error)) {
        state.was_active = false;
        report_problem(state, L"no-owner", L"watch: " + error);
        return 1;
    }
    return watch_round_in(context, options, state, nullptr);
}

int do_watch(Options const& options) {
    std::wstring const dll_path = payload_path_for(options);
    if (!file_exists(dll_path)) {
        report(L"payload DLL not found: " + dll_path);
        return 1;
    }

    report(L"watch: every " + std::to_wstring(options.interval_seconds) + L" s, scope=\"" +
           options.scope + L"\"" + (options.once ? L", one round only" : L""));

    HANDLE stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop == nullptr) {
        report(L"watch: cannot create the stop event: " + deskfeed::last_error_text());
        return 1;
    }
    g_watch_stop = stop;
    ::SetConsoleCtrlHandler(watch_console_handler, TRUE);

    WatchState state;
    int code = 0;
    bool ended_unknown = false;
    for (;;) {
        ActionContext context;
        std::wstring target_error;
        if (!resolve_action_target(context, target_error)) {
            state.was_active = false;
            report_problem(state, L"no-owner", L"watch: " + target_error);
            code = 1;
        } else {
            g_watch_busy.store(true);
            code = watch_round_in(context, options, state, stop);
            g_watch_busy.store(false);
            if (code == kWatchRoundLostUncertain) {
                // 本线程必须结束：不再进入下一轮，也不再 acquire。
                ended_unknown = true;
                report(L"watch: the round ended with an unproven entry thread; the watch exits so "
                       L"that this thread really ends (a later run recovers the abandoned lease)");
                break;
            }
        }

        if (options.once) {
            break;
        }
        if (::WaitForSingleObject(stop, options.interval_seconds * 1000) == WAIT_OBJECT_0) {
            report(L"watch: a stop was requested; the payload stays where it is (run --stop to "
                   L"remove it)");
            break;
        }
    }

    g_watch_busy.store(false);
    g_watch_stop = nullptr;
    ::SetConsoleCtrlHandler(watch_console_handler, FALSE);
    ::CloseHandle(stop);
    report(L"watch: stopped");
    if (ended_unknown) {
        return 1;
    }
    return options.once ? code : 0;
}

}  // namespace deskfeed::inject
