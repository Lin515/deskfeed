#include "inject/actions.h"

#include "inject/payload_status.h"
#include "inject/pe_export.h"
#include "inject/target.h"
#include "platform/log.h"
#include "platform/win32_util.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

namespace deskfeed::inject {
namespace {

// 一次 `DeskFeedStop` 允许等待的毫秒数。负载内部对非法/0 值也用 10 秒兜底，两处一致。
constexpr unsigned kStopTimeoutMs = 10000;
// 调用远程导出的等待上限：留给它自己处理，再宽一点。
constexpr DWORD kCallGraceMs = 5000;

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

// 把 scope 写进目标进程的一块缓冲，调用 Start 之后再释放。**只有远程线程确实退出后**才释放：
// 负载在参数上复制一份 scope 之后才返回，超时释放会让它读到已释放的内存。
bool call_start(std::wstring const& scope, unsigned long process_id, unsigned long long module_base,
                unsigned long export_rva, RemoteCall& call, std::wstring& error) {
    RemoteBuffer buffer;
    bool allocated = false;
    if (!scope.empty()) {
        std::size_t const bytes = (scope.size() + 1) * sizeof(wchar_t);
        if (!remote_alloc(process_id, bytes, buffer, error)) {
            return false;
        }
        allocated = true;
        if (!remote_write(process_id, buffer.address, scope.c_str(), bytes, error)) {
            remote_free(process_id, buffer);
            return false;
        }
    }

    call = call_remote_export(process_id, module_base, export_rva,
                              allocated ? reinterpret_cast<void*>(buffer.address) : nullptr, 15000);

    if (allocated) {
        if (call.signaled) {
            remote_free(process_id, buffer);
        } else {
            // 不释放：负载线程可能还在读它。这块内存会随目标进程退出归还，比读已释放的内存安全。
            report(L"note: the remote scope buffer stays allocated because the Start thread has not "
                   L"exited; it is released when the target process exits");
        }
    }
    return true;
}

// 从一个进程里撤下负载：远程 `DeskFeedStop` → 等远程线程退出 → 只有报"可以卸载"才 `FreeLibrary`。
// 返回 false 时**模块仍留在目标进程里**（未退休、超时、或远程调用失败）。模块本来就不在时算成功
// （"已经撤下"），但"读不到模块表"是失败：不能把不可检查当成已经撤下。
bool stop_one(unsigned long process_id, std::wstring const& dll_path, bool no_unload, bool& retired,
              std::wstring& error) {
    retired = false;

    ModuleEntry module;
    std::wstring lookup_error;
    switch (lookup_module_by_full_path(process_id, dll_path, module, lookup_error)) {
        case ModuleLookup::Found:
            break;
        case ModuleLookup::NotFound:
            report(L"payload is not loaded in pid " + std::to_wstring(process_id) + L" (" +
                   lookup_error + L")");
            retired = true;
            return true;
        case ModuleLookup::Error:
            error = L"cannot read the module list of pid " + std::to_wstring(process_id) + L": " +
                    lookup_error;
            return false;
    }
    report(L"payload loaded in pid " + std::to_wstring(process_id) + L": base=" +
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
    if (!verify_remote_export(process_id, module.base, module.size, kStopExportName, info,
                              verified_rva, error)) {
        error = L"the loaded payload does not match the on-disk DLL: " + error;
        return false;
    }

    // 超时值按 uintptr_t 编码在参数指针本身里（不解引用），这是冻结的负载 ABI。
    void* const encoded_timeout = reinterpret_cast<void*>(static_cast<std::uintptr_t>(kStopTimeoutMs));
    RemoteCall const call =
        call_remote_export(process_id, module.base, verified_rva, encoded_timeout,
                           kStopTimeoutMs + kCallGraceMs);

    UnloadDecision const decision = unload_decision(call.exit_code, call.signaled, no_unload);
    if (decision == UnloadDecision::KeepLoaded) {
        if (!call.thread_created) {
            error = L"DeskFeedStop was not called: " + call.error;
        } else if (!call.signaled) {
            error = L"the DeskFeedStop remote thread has not exited within " +
                    std::to_wstring(kStopTimeoutMs + kCallGraceMs) +
                    L" ms; keeping the module mapped";
        } else {
            error = L"DeskFeedStop reported that the payload is not retired (exit code " +
                    deskfeed::hex_text(call.exit_code) + L"); keeping the module mapped";
        }
        return false;
    }
    if (decision == UnloadDecision::KeepNoUnload) {
        report(L"the payload acknowledged the stop; leaving the DLL mapped (--no-unload)");
        retired = true;
        return true;
    }

    report(L"the payload retired; unloading the module");
    if (!eject_library(process_id, module.base, error)) {
        return false;
    }
    retired = true;
    return true;
}

// 别的 explorer.exe 里可能还留着一份负载（以前装错过进程，或者旧 explorer 还没退）。留着它
// 会一直占着 DLL、也会在下一次构建时报 LNK1168，而且它自己那侧永远插不出读数块。
// 返回 false 表示"至少有一份没能安全撤下"。
bool clean_other_explorers(unsigned long owner_pid, std::wstring const& dll_path, bool dry_run) {
    std::vector<ProcessEntry> explorers;
    std::wstring error;
    if (!list_processes_by_image(L"explorer.exe", explorers, error)) {
        report(L"cannot enumerate explorer.exe processes: " + error);
        return true;  // 枚举失败不算"有残留没清掉"
    }

    bool all_ok = true;
    for (ProcessEntry const& other : explorers) {
        if (other.process_id == owner_pid) {
            continue;
        }
        ModuleEntry stale;
        std::wstring stale_error;
        switch (lookup_module_by_full_path(other.process_id, dll_path, stale, stale_error)) {
            case ModuleLookup::Found:
                break;
            case ModuleLookup::NotFound:
                continue;
            case ModuleLookup::Error:
                report(L"cannot inspect pid " + std::to_wstring(other.process_id) +
                       L" for a stale payload: " + stale_error);
                continue;
        }
        report(L"another explorer.exe (pid " + std::to_wstring(other.process_id) +
               L") also has the payload loaded at " + deskfeed::hex_text(stale.base) + L"");
        if (dry_run) {
            report(L"would stop and unload it");
            continue;
        }
        bool retired = false;
        std::wstring stop_error;
        if (!stop_one(other.process_id, dll_path, /*no_unload=*/false, retired, stop_error)) {
            report(L"could not stop the payload in pid " + std::to_wstring(other.process_id) + L": " +
                   stop_error);
            all_ok = false;
        }
    }
    return all_ok;
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

int do_load(Options const& options) {
    std::wstring const dll_path = payload_path_for(options);
    if (!file_exists(dll_path)) {
        report(L"payload DLL not found: " + dll_path);
        return 1;
    }
    report(L"payload: " + dll_path);

    ProcessEntry target;
    std::wstring target_error;
    if (!resolve_taskbar_owner(target, target_error)) {
        report(target_error);
        return 1;
    }
    report(L"target explorer.exe pid=" + std::to_wstring(target.process_id) + L" image=" +
           target.image_path + L" scope=\"" + options.scope + L"\"");

    // 位数的硬门禁：x64 负载装不进 32 位（WOW64）任务栏属主，读不到架构也拒绝。
    std::wstring arch_error;
    if (!target_is_native_x64(target.process_id, arch_error)) {
        report(arch_error);
        return 1;
    }

    PayloadStatus status;
    std::wstring status_error;
    if (!query_payload_status(target.process_id, dll_path, options.scope, status, status_error)) {
        report(L"cannot query the payload state: " + status_error);
        return 1;
    }
    report(std::wstring(L"current payload state: ") + ascii_to_wide(payload_state_name(status.state)) +
           L" (" + status.detail + L")");

    if (status.mapped) {
        if (status.state == PayloadState::MappedRetained) {
            report(L"refusing to reload: the mapped payload cannot prove it is retired (" +
                   status.detail + L"); fix or stop it first");
            // 目标这一份不能动（它连"已退休"都证明不了），但别的 explorer.exe 里的残留仍要按同一套
            // 安全门撤下：每次 load 都要做，哪怕这一轮对本目标什么都不做。
            if (options.dry_run) {
                report(L"dry run: would still retire stale payloads in other explorer.exe processes");
            } else if (!clean_other_explorers(target.process_id, dll_path, /*dry_run=*/false)) {
                report(L"stale-other-retained: a stale payload in another explorer.exe could not be safely removed (deployment requires no stale payload elsewhere)");
            }
            return 1;
        }
        if (status.state == PayloadState::MappedActive) {
            // 本目标这一轮是 no-op，但"清别的 explorer 里的残留"不是 no-op：旧 explorer 没退、
            // 或以前装错过进程时，那一份会一直占着 DLL 且永远插不出读数块。stop_one 只在负载报
            // "已退休"且远程线程退出后才 FreeLibrary，超时保留映射、绝不按镜像名杀进程。
            report(L"the payload is already running on this scope and bound to the live producer; "
                   L"nothing to do for this target");
            if (options.dry_run) {
                report(L"dry run: would still retire stale payloads in other explorer.exe processes");
                return 0;
            }
            if (!clean_other_explorers(target.process_id, dll_path, /*dry_run=*/false)) {
                report(L"stale-other-retained: a stale payload in another explorer.exe could not be safely removed (deployment requires no stale payload elsewhere)");
            }
            report(L"nothing was injected for this target");
            return 0;
        }
        if (options.dry_run) {
            report(L"would stop the mapped payload (it is not provably active) and load it again");
            report(L"dry run: would also retire stale payloads in other explorer.exe processes");
            return 0;
        }
        report(L"the mapped payload is not provably active; removing it before loading again");
        bool retired = false;
        std::wstring stop_error;
        if (!stop_one(target.process_id, dll_path, /*no_unload=*/false, retired, stop_error)) {
            report(L"cannot remove the stale payload: " + stop_error);
            return 1;
        }
    }

    if (options.dry_run) {
        report(L"would inject " + dll_path + L" into pid " + std::to_wstring(target.process_id) +
               L" and call " + ascii_to_wide(kStartExportName) + L" with scope \"" +
               options.scope + L"\"");
        report(L"dry run: nothing was injected");
        return 0;
    }

    if (!clean_other_explorers(target.process_id, dll_path, /*dry_run=*/false)) {
        // 不把它当成"清干净了"：如实记下 stale-other-retained。本目标的装载继续（失败的是别的
        // explorer 里那份，不影响这里），但部署门禁要求最终别处没有旧负载。
        report(L"stale-other-retained: a stale payload in another explorer.exe could not be safely "
               L"removed; continuing with this target (deployment requires no stale payload elsewhere)");
    }

    // 破坏性动作之前再核对一次身份：这中间如果任务栏属主重启了（explorer 崩溃是常态），PID 可能
    // 已经被别的进程复用，刚才解析出来的目标就不再是同一个进程。
    if (target.creation_time != 0 &&
        !process_matches_identity(target.process_id, target.creation_time)) {
        report(L"the taskbar owner changed identity while preparing the load; aborting without "
               L"injecting");
        return 1;
    }

    ModuleEntry loaded;
    bool already_loaded = false;
    std::wstring inject_error;
    if (!inject_library(target.process_id, dll_path, loaded, already_loaded, inject_error)) {
        report(L"injection failed: " + inject_error);
        return 1;
    }
    if (already_loaded) {
        report(L"the payload is already mapped in the target (another injector won the race); "
               L"run '--status' to see whether it is active");
        return 1;
    }
    report(L"payload loaded: base=" + deskfeed::hex_text(loaded.base) + L" size=" +
           deskfeed::hex_text(loaded.size) + L" path=" + loaded.path);

    PeExportError kind = PeExportError::None;
    PeExportInfo start_info;
    std::wstring export_error;
    if (!pe_export_rva(dll_path, kStartExportName, start_info, export_error, &kind)) {
        report(L"cannot locate " + ascii_to_wide(kStartExportName) + L": " + export_error);
        return 1;
    }
    if (!pe_image_matches_module(start_info, loaded.size, export_error)) {
        report(L"cannot use the on-disk export offsets: " + export_error);
        return 1;
    }
    // 尺寸相同不等于同一个 build：读一次远程镜像自己的 PE 元数据/导出表，用远程确认过的 RVA。
    unsigned long start_rva = 0;
    if (!verify_remote_export(target.process_id, loaded.base, loaded.size, kStartExportName, start_info,
                              start_rva, export_error)) {
        report(L"the loaded payload does not match the on-disk DLL; refusing to call into it: " +
               export_error);
        return 1;
    }

    RemoteCall start_call;
    std::wstring start_error;
    if (target.creation_time != 0 &&
        !process_matches_identity(target.process_id, target.creation_time)) {
        report(L"the taskbar owner changed identity before the Start call; the payload stays "
               L"mapped but is not started");
        return 1;
    }
    if (!call_start(options.scope, target.process_id, loaded.base, start_rva, start_call,
                    start_error)) {
        report(L"cannot prepare the Start call: " + start_error);
        return 1;
    }
    if (!start_call.thread_created) {
        report(L"the Start call failed: " + start_call.error);
        return 1;
    }
    if (!start_call.signaled) {
        report(L"the Start thread has not exited within the timeout; the payload may still be "
               L"starting. Check '--status' before trying again");
        return 1;
    }

    switch (start_call.exit_code) {
        case 1:
            report(L"injection succeeded: the payload reported that it started");
            return 0;
        case 2:
            report(L"the payload rejected the request (bad scope pointer or a scope that is not on "
                   L"the whitelist)");
            return 1;
        case 3:
            report(L"the payload reports that it is already running");
            if (query_payload_status(target.process_id, dll_path, options.scope, status,
                                     status_error) &&
                status.state == PayloadState::MappedActive) {
                report(L"and it is provably active on this scope; treating the load as satisfied");
                return 0;
            }
            report(L"but it cannot be proven active on this scope (" + status.detail +
                   L"); stopping it is the safe next step");
            return 1;
        case 4:
            report(L"the payload could not start its worker thread");
            return 1;
        default:
            report(L"the payload's Start entry point returned an unknown code: " +
                   deskfeed::hex_text(start_call.exit_code));
            return 1;
    }
}

int do_stop(Options const& options) {
    std::wstring const dll_path = payload_path_for(options);
    if (!file_exists(dll_path)) {
        report(L"payload DLL not found: " + dll_path);
        return 1;
    }

    ProcessEntry target;
    std::wstring target_error;
    if (!resolve_taskbar_owner(target, target_error)) {
        report(target_error);
        return 1;
    }
    report(L"target explorer.exe pid=" + std::to_wstring(target.process_id) + L" image=" +
           target.image_path + L" scope=\"" + options.scope + L"\"");

    // 目标进程 + 别的 explorer.exe 里的残留，一份都不留。没装载的进程按"已经撤下"处理。
    std::vector<unsigned long> victims;
    victims.push_back(target.process_id);

    std::vector<ProcessEntry> explorers;
    std::wstring list_error;
    if (list_processes_by_image(L"explorer.exe", explorers, list_error)) {
        for (ProcessEntry const& other : explorers) {
            if (other.process_id == target.process_id) {
                continue;
            }
            ModuleEntry stale;
            std::wstring stale_error;
            switch (lookup_module_by_full_path(other.process_id, dll_path, stale, stale_error)) {
                case ModuleLookup::Found:
                    report(L"another explorer.exe (pid " + std::to_wstring(other.process_id) +
                           L") also has the payload loaded; stopping it too");
                    victims.push_back(other.process_id);
                    break;
                case ModuleLookup::NotFound:
                    break;
                case ModuleLookup::Error:
                    report(L"cannot inspect pid " + std::to_wstring(other.process_id) + L": " +
                           stale_error);
                    break;
            }
        }
    } else {
        report(L"cannot enumerate explorer.exe processes: " + list_error);
    }

    bool all_ok = true;
    for (unsigned long const pid : victims) {
        ModuleEntry module;
        std::wstring find_error;
        switch (lookup_module_by_full_path(pid, dll_path, module, find_error)) {
            case ModuleLookup::NotFound:
                report(L"payload is not loaded in pid " + std::to_wstring(pid) +
                       L" (already removed)");
                continue;
            case ModuleLookup::Error:
                report(L"cannot read the module list of pid " + std::to_wstring(pid) + L": " +
                       find_error);
                all_ok = false;
                continue;
            case ModuleLookup::Found:
                break;
        }
        if (options.dry_run) {
            report(L"would stop the payload in pid " + std::to_wstring(pid) +
                   (options.unload ? L" and unload the DLL" : L" and leave the DLL mapped"));
            continue;
        }
        bool retired = false;
        std::wstring stop_error;
        if (!stop_one(pid, dll_path, options.unload, retired, stop_error)) {
            report(L"could not stop the payload in pid " + std::to_wstring(pid) + L": " + stop_error);
            all_ok = false;
            continue;
        }
        report(L"the payload in pid " + std::to_wstring(pid) +
               (options.unload ? L" was stopped and the DLL unloaded" : L" was stopped (DLL kept)"));
    }

    if (options.dry_run) {
        report(L"dry run: nothing was changed");
        return all_ok ? 0 : 1;
    }
    return all_ok ? 0 : 1;
}

int do_status(Options const& options) {
    std::wstring const dll_path = payload_path_for(options);

    ProcessEntry target;
    std::wstring target_error;
    if (!resolve_taskbar_owner(target, target_error)) {
        report(target_error);
        report(L"status: cannot tell (no taskbar owner)");
        return 2;
    }

    PayloadStatus status;
    std::wstring status_error;
    if (!query_payload_status(target.process_id, dll_path, options.scope, status, status_error)) {
        report(L"status: cannot tell (" + status_error + L")");
        return 2;
    }

    report(L"status: " + ascii_to_wide(payload_state_name(status.state)) + L" pid=" +
           std::to_wstring(target.process_id) + L" scope=\"" + options.scope + L"\"");
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
        report(L"mailbox producer: pid=" + std::to_wstring(status.mailbox_producer_pid) + L" alive=" +
               (status.mailbox_producer_alive ? L"yes" : L"no") + L" consistent=" +
               (status.producer_consistent ? L"yes" : L"no"));
    }

    switch (status.state) {
        case PayloadState::MappedActive:
            return 0;
        case PayloadState::MappedRetained:
            report(L"status: the payload cannot be safely reloaded; not loaded is NOT the answer "
                   L"here (exit 2)");
            return 2;
        case PayloadState::NotMapped:
        case PayloadState::MappedInactive:
        case PayloadState::MappedUnknownAbi:
            return 1;
    }
    return 2;
}

namespace {

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

}  // namespace

int watch_round(Options const& options, WatchState& state) {
    std::wstring const dll_path = payload_path_for(options);

    ProcessEntry target;
    std::wstring target_error;
    if (!resolve_taskbar_owner(target, target_error)) {
        state.was_active = false;
        report_problem(state, L"no-owner", L"watch: " + target_error);
        return 1;
    }
    if (state.pid != target.process_id) {
        if (state.pid != 0) {
            report(L"watch: the taskbar owner changed (pid " + std::to_wstring(state.pid) + L" -> " +
                   std::to_wstring(target.process_id) + L")");
        } else {
            report(L"watch: taskbar owner pid=" + std::to_wstring(target.process_id) + L" image=" +
                   target.image_path);
        }
        state.pid = target.process_id;
        state.was_active = false;
        state.problem.clear();
        state.next_attempt_ms = 0;
    }

    PayloadStatus status;
    std::wstring status_error;
    if (!query_payload_status(target.process_id, dll_path, options.scope, status, status_error)) {
        report_problem(state, L"query", L"watch: cannot query the payload state: " + status_error);
        return 1;
    }

    if (status.state == PayloadState::MappedActive) {
        if (!state.was_active) {
            report(L"watch: the payload is active on this scope");
        }
        state.was_active = true;
        state.problem.clear();
        state.next_attempt_ms = 0;
        return 0;
    }

    if (status.state == PayloadState::MappedRetained) {
        state.was_active = false;
        report_problem(state, L"retained",
                       L"watch: the mapped payload is not retired (" + status.detail +
                           L"); waiting instead of forcing an unload");
        return 1;
    }

    if (options.dry_run) {
        report_problem(state, L"dry",
                       L"watch: would load the payload (" + status.detail + L")");
        state.was_active = false;
        return 0;
    }

    if (state.next_attempt_ms != 0 && ::GetTickCount64() < state.next_attempt_ms) {
        return 1;
    }

    report_problem(state, L"load", L"watch: loading the payload (" + status.detail + L")");
    // 每一轮都重新解析：--load 自己会做"残留先撤下、状态复查"的全部步骤。
    int const code = do_load(options);
    if (code != 0) {
        state.was_active = false;
        state.next_attempt_ms = ::GetTickCount64() + 30000;
        report_problem(state, L"load-failed",
                       L"watch: the load did not succeed; the next attempt is 30 s later");
        return 1;
    }
    state.was_active = true;
    state.problem.clear();
    state.next_attempt_ms = 0;
    return 0;
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
    for (;;) {
        g_watch_busy.store(true);
        code = watch_round(options, state);
        g_watch_busy.store(false);

        if (options.once) {
            break;
        }
        if (::WaitForSingleObject(stop, options.interval_seconds * 1000) == WAIT_OBJECT_0) {
            report(L"watch: a stop was requested; the payload stays where it is (run --stop to "
                   L"remove it)");
            break;
        }
    }

    g_watch_stop = nullptr;
    ::SetConsoleCtrlHandler(watch_console_handler, FALSE);
    ::CloseHandle(stop);
    report(L"watch: stopped");
    return options.once ? code : 0;
}

}  // namespace deskfeed::inject
