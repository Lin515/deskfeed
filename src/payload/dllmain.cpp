// 注入进 explorer.exe 的负载 DLL 的入口。
//
// 两个产物、一份源：生产 `deskfeed-payload.dll` 与自测 `deskfeed-payload-test.dll` 用的是同一份源
// 列表、同一清单、同一 CFG/MAP，唯一差异是后者多一个 `DESKFEED_DLL_TEST_API`（见 payload_flavor.h）。
// 探针入口与全部对抗开关只在自测构建里编译，生产 DLL 里连入口都不存在。
//
// DllMain 只做最小的事：记住模块句柄、关掉线程通知、准备清理事件。其余全部在导出的入口起的
// 工作线程上跑——在 DllMain 里创建线程会在持有加载器锁时运行它。
//
// 导出的 ABI 是**给 CreateRemoteThread 用的**，所以每个都返回 `DWORD WINAPI f(void*)`：
// 返回 void 会让线程退出码变成未定义值，调用方无从判断成功。
//   DeskFeedStart(void* scope_or_null)   1 已启动 / 2 请求非法 / 3 已在运行 / 4 起线程失败
//   DeskFeedStop(void* timeout_ms)       1 已退休可以卸载 / 0 未退休（必须保留映射）
// 超时值按 `uintptr_t` 编码在参数指针本身里（不解引用），这样注入器不必在目标进程里再写一个 DWORD。
#include "display/reading.h"
#include "payload/action_worker.h"
#include "payload/guard_keeper.h"
#include "payload/payload_flavor.h"
#include "payload/hook_post.h"
#include "payload/host.h"
#include "payload/reg_hook.h"
#include "payload/status_pod.h"
#include "payload/taskbar_xaml.h"
#include "payload/xaml_tap.h"
#include "platform/log.h"
#include "platform/paths.h"

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
#include "payload/probe_api.h"
#endif

#include <windows.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <string>

namespace {

HINSTANCE g_module = nullptr;
std::atomic<bool> g_started{false};

DWORD WINAPI payload_thread_entry(LPVOID parameter) {
    // 这是负载线程的最外层。`payload_run` 自己还有 C++/SEH 两层边界，这里再兜一次：连"复制
    // scope 字符串"这种在边界之前发生的事也不能让异常穿回 kernel32（那就是 explorer 终止）。
    std::wstring const* scope = static_cast<std::wstring const*>(parameter);
    try {
        deskfeed::payload::payload_run(g_module, scope != nullptr ? *scope : std::wstring());
    } catch (...) {
    }
    try {
        delete scope;
    } catch (...) {
    }
    return 0;
}

// 远程传进来的 scope 指针指向目标进程里的一块缓冲（由注入器写好），但本模块仍然要有界、要防
// 坏指针。SEH 外壳里不得有需要析构的局部对象。
bool copy_scope_seh(void const* pointer, wchar_t* buffer, std::size_t capacity) {
    __try {
        for (std::size_t index = 0; index < capacity; ++index) {
            wchar_t const value = static_cast<wchar_t const*>(pointer)[index];
            buffer[index] = value;
            if (value == L'\0') {
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    buffer[capacity - 1] = L'\0';
    return true;
}

std::wstring read_scope(void* pointer, bool& ok) {
    ok = true;
    if (pointer == nullptr) {
        return std::wstring();  // 生产实例
    }
    wchar_t buffer[64]{};
    if (!copy_scope_seh(pointer, buffer, std::size(buffer))) {
        ok = false;
        return std::wstring();
    }
    buffer[std::size(buffer) - 1] = L'\0';
    std::wstring scope(buffer);
    std::wstring error;
    if (!deskfeed::scope_validate(scope, error) && !scope.empty()) {
        ok = false;
        return std::wstring();
    }
    return scope;
}

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
void probe_placeholder_reading() {
    deskfeed::ReadingContent content;
    deskfeed::ReadingRow first;
    first.label = "probe";
    first.value = "waiting for a frame";
    first.tone = deskfeed::ReadingTone::Normal;
    first.tip = "the probe host inserts this until a frame arrives";
    content.rows.push_back(first);
    deskfeed::ReadingRow second;
    second.label = "mode";
    second.value = "structural self-test";
    second.tone = deskfeed::ReadingTone::Warn;
    content.rows.push_back(second);
    deskfeed::payload::taskbar_xaml_set_reading(content);
}

// 探针开关的统一复位。探针拆除之后都必须回到这里的状态：完成事件变异关闭、阶段记录关闭、托盘窗口
// 回退到 `FindWindow(Shell_TrayWnd)`。（生产 DLL 里没有这些开关，所以也没有这段。）
void reset_probe_state() {
    deskfeed::payload::hook_post_probe_set_completion_mutation(
        deskfeed::payload::CompletionMutation::Normal);
    deskfeed::payload::hook_post_probe_set_observer(false);
    deskfeed::payload::hook_post_probe_set_tray_window(nullptr);
}
#endif  // DESKFEED_PAYLOAD_PROBE_BUILD

// 状态 POD 的受控拷贝（叶子，不携带析构局部对象）：坏指针由 SEH 吃掉。
bool copy_payload_status_seh(deskfeed::payload::PayloadStatusPod const& source, void* destination,
                             unsigned bytes) {
    __try {
        std::memcpy(destination, &source, bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// 读调用方写好的两个头字段（坏指针由 SEH 吃掉）。`__try` 与 C++ 对象展开不能共存，所以这些
// 都必须是小叶子。
bool read_status_header_seh(void* destination, std::uint32_t& size, std::uint32_t& abi) {
    __try {
        auto* const pod = reinterpret_cast<deskfeed::payload::PayloadStatusPod*>(destination);
        size = pod->size;
        abi = pod->abi;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void write_status_abi_zero_seh(void* destination) {
    __try {
        auto* const pod = reinterpret_cast<deskfeed::payload::PayloadStatusPod*>(destination);
        pod->abi = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
// 把负载内部状态写进探针给的 POD。**先**在这个不携带析构局部对象的叶子里做受控拷贝（坏指针由
// SEH 吃掉），再把拷贝好的值交回去——绝不把内部地址/对象交给探针。
bool copy_status_seh(deskfeed::payload::HookProbeStatus const& source, void* destination,
                     unsigned bytes) {
    __try {
        std::memcpy(destination, &source, bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool copy_stage_seh(deskfeed::payload::HookProbeStage const& source, void* destination,
                    unsigned bytes) {
    __try {
        std::memcpy(destination, &source, bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}
#endif  // DESKFEED_PAYLOAD_PROBE_BUILD

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI DeskFeedStart(void* scope_param) {
    try {
        bool scope_ok = false;
        std::wstring const scope = read_scope(scope_param, scope_ok);
        if (!scope_ok) {
            return 2;
        }
        if (g_module == nullptr) {
            return 4;
        }

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
        // 自测构建：先把探针的开关全部复位，避免任何人误以为探针配置能改变生产行为。
        // 例外：自测宿主显式"布防"过（R5 指定的托盘窗口必须活过 Start，真实 mailbox 的 UI 投递
        // 才能落在它那条线程上）。布防只可能来自同一进程里的探针调用。
        //
        // 生产 DLL 里没有这段：那些开关本身不存在，所以也不存在"caller 用探针改变生产行为"这条路。
        if (!deskfeed::payload::hook_post_probe_armed()) {
            deskfeed::payload::hook_post_probe_set_completion_mutation(
                deskfeed::payload::CompletionMutation::Normal);
            deskfeed::payload::hook_post_probe_set_observer(false);
            deskfeed::payload::hook_post_probe_set_tray_window(nullptr);
        }
#endif

        bool expected = false;
        if (!g_started.compare_exchange_strong(expected, true)) {
            return 3;
        }
        // 状态快照的 scope 只在这里登记，而且是在**线程发布之前**：查询方永远没有机会指定 scope，
        // 也不可能读到"上一代的 scope 配这一代的身份"这种混合。它同时开一代（清掉上一代的回收门）。
        deskfeed::payload::payload_publish_scope(scope);
        auto* payload_scope = new (std::nothrow) std::wstring(scope);
        if (payload_scope == nullptr) {
            g_started.store(false);
            return 4;
        }
        // CREATE_SUSPENDED + 先登记句柄 + 再 Resume：负载线程在句柄可见之前不许跑。
        // 否则"刚 Start 就 Stop"会看到 nullptr 句柄 ⇒ 在一条本模块线程已经在跑的情况下报告
        // "可以卸载"。句柄所有权登记之后归负载（见 payload_publish_thread），这里不关。
        HANDLE const thread = ::CreateThread(nullptr, 0, payload_thread_entry, payload_scope,
                                             CREATE_SUSPENDED, nullptr);
        if (thread == nullptr) {
            delete payload_scope;
            g_started.store(false);
            return 4;
        }
        if (!deskfeed::payload::payload_publish_thread(thread)) {
            ::CloseHandle(thread);
            delete payload_scope;
            g_started.store(false);
            return 4;
        }
        if (::ResumeThread(thread) == static_cast<DWORD>(-1)) {
            // 登记成功但起不来。句柄的所有权此刻已经归负载（`g_payload_thread`）：**绝不能**在这里
            // `CloseHandle` —— 那会让状态查询与 Stop 去等一个已关闭、可能被复用的句柄值。线程也仍然
            // 是 SUSPENDED、永远不会自己退出。
            //
            // 所以这条路径按 fail closed 处理：保留真实句柄与登记，把"这一次 Start 没有被接受"
            // 明确写回状态快照（`started = 0`，`thread_alive` 仍由真实句柄决定），由 Stop 去等这条
            // 永远不触发的句柄并返回"未退休"。绝不 `TerminateThread`，也绝不在没有"线程真的终止"
            // 证据的情况下清登记。
            deskfeed::payload::payload_mark_start_rejected();
            return 4;
        }
        return 1;
    } catch (...) {
        // 这条入口由注入器的 CreateRemoteThread 调用：异常穿出就是 explorer 终止。
        // 语义保持三态：请求非法返回 2（不要返回"已启动"）。
        return 2;
    }
}

extern "C" __declspec(dllexport) DWORD WINAPI DeskFeedStop(void* timeout_param) {
    try {
        // **第一道门**（在 `payload_request_stop()` 之前）：不可证就保留映射。
        //   * 粘性未知（DllMain 没拿到/验不过那三个内核对象，或视图不可读）；
        //   * 粘性不确定事件已置位（actor 在不可证的事实下置的，Native **永不**替它清）；
        //   * 记录不可变头（size/version）不认。
        // 这里**不读** actor 快照：actor 自己发起这次 Stop 之前按写序已经写下 `Pending*`，那正是要
        // 允许走到结论的那次调用。事件等待的结论也必须显式区分 WAIT_TIMEOUT——"不是已置位"不等于
        // "干净"，WAIT_FAILED 同样不可证。
        if (!deskfeed::payload::payload_guard_stop_allowed()) {
            return 0;
        }

        unsigned timeout = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(timeout_param));
        if (timeout == 0 || timeout > 60000) {
            timeout = 10000;
        }

        deskfeed::payload::payload_request_stop();

        // 卸载门是**线程已终止**，不是事件、不是标志位：事件之后线程入口还有一条 `return`，那也是
        // 本模块的指令。
        HANDLE const thread = deskfeed::payload::payload_thread_handle();
        if (thread != nullptr) {
            if (::WaitForSingleObject(thread, timeout) != WAIT_OBJECT_0) {
                return 0;
            }
        } else if (deskfeed::payload::action_worker_running()) {
            // 句柄没登记过，但动作工作线程还在跑：那也是一条本模块的线程 ⇒ 绝不能报告可卸载。
            return 0;
        }

        // 到这里线程确实已经终止（或从未起过）。负载线程自己的收尾若还有门没过（例如动作工作
        // 线程在它的超时里没退出、钩子的 ping 没拿到），现在由这里把没过的门再走一遍：同一时刻
        // 只有一个驱动者，且不在持锁期间做任何等待。已经拿到的门不会重跑。
        std::wstring detail;
        if (!deskfeed::payload::payload_retire_after_thread(timeout, detail)) {
            deskfeed::log_format(L"payload stays loaded (not retired): %s", detail.c_str());
            return 0;
        }
        return 1;
    } catch (...) {
        // 未退休：注入器必须保留映射（绝不在这里报告"可以卸载"）。
        return 0;
    }
}

// 本模块同时充当 XAML 诊断的 TAP，所以 COM 的这两个入口也在它身上。
extern "C" __declspec(dllexport) HRESULT __stdcall DllGetClassObject(REFCLSID class_id,
                                                                    REFIID interface_id,
                                                                    void** object) {
    return deskfeed::payload::tap_get_class_object(class_id, interface_id, object);
}

// 永远不答应卸载。XAML 只在它持有 TAP 对象时问这个，而模块是被注入的负载，何时卸载由注入器决定；
// 在这里答应会把模块从负载自己的线程脚下抽走。
extern "C" __declspec(dllexport) HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
// ---------------------------------------------------------------------------
// 自测宿主（deskfeed-probe.exe）用的入口。它们让诊断这条路线的全部危险动作在一个自己拥有 XAML
// 岛的进程里先跑一遍，不必注入 explorer。
//
// 整段只在自测构建里编译：生产 `deskfeed-payload.dll` 的导出表里**没有**任何 `DeskFeedProbe*`，
// 所以外部进程无法用探针入口改变它的行为。审计（`--audit`）会把这一条当硬断言检查。
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeStart() {
    std::wstring error;
    if (!deskfeed::payload::xaml_tap_start(error)) {
        deskfeed::log_format(L"probe: diagnostics start failed: %s", error.c_str());
        return -1;
    }
    return static_cast<int>(deskfeed::payload::xaml_tap_sites().size());
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeSites() {
    return static_cast<int>(deskfeed::payload::xaml_tap_sites().size());
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeWatch() {
    std::wstring error;
    if (!deskfeed::payload::taskbar_xaml_watch(error)) {
        deskfeed::log_format(L"probe: watch failed: %s", error.c_str());
        return 0;
    }
    return 1;
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeRun(int dump_only) {
    std::wstring error;
    bool visible = false;
    if (!deskfeed::payload::taskbar_xaml_probe_run(dump_only != 0, /*allow_placeholder=*/true, visible,
                                                   error)) {
        deskfeed::log_format(L"probe: run failed: %s", error.c_str());
        return 0;
    }
    return 1;
}

// 宿主线程就是树所属的线程：告诉负载不必再投递（也没有 Shell_TrayWnd 可投），并把宿主窗口交给
// 它当悬停提示的属主与钩子收尾的确认窗口。
extern "C" __declspec(dllexport) void __stdcall DeskFeedProbeSetHost(HWND window) {
    deskfeed::payload::taskbar_xaml_set_probe_inline(true);
    deskfeed::payload::taskbar_xaml_set_host_window(window);
    probe_placeholder_reading();
}

extern "C" __declspec(dllexport) void __stdcall DeskFeedProbeSetSlowApply(unsigned ms) {
    deskfeed::payload::taskbar_xaml_probe_set_slow_apply(ms);
}

// 走真实的"投递到 UI 线程 + 收尾证明"这条路径（钩子门）。
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbePostApply(unsigned thread_id,
                                                                     unsigned timeout_ms) {
    auto const posted =
        deskfeed::payload::hook_post_run(thread_id, deskfeed::payload::HookAction::Apply, timeout_ms);
    if (!posted.ok && !posted.error.empty()) {
        deskfeed::log_format(L"probe: post failed: %s", posted.error.c_str());
    }
    return posted.ok && posted.retired ? 1 : 0;
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeRetire(unsigned timeout_ms) {
    std::wstring detail;
    if (!deskfeed::payload::hook_post_retire(nullptr, 0, timeout_ms, detail)) {
        deskfeed::log_format(L"probe: retire failed: %s", detail.c_str());
        return 0;
    }
    return 1;
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeHook(int enable) {
    if (enable == 0) {
        std::wstring error;
        return deskfeed::payload::reg_hook_restore(error) ? 1 : 0;
    }
    std::wstring detail;
    bool const before = deskfeed::payload::reg_hook_probe_read(detail);
    deskfeed::log_format(L"probe: hook selftest before install (%s): %s", before ? L"passed" : L"failed",
                         detail.c_str());
    std::wstring error;
    if (!deskfeed::payload::reg_hook_install(error)) {
        deskfeed::log_format(L"probe: cannot install the reg hook: %s", error.c_str());
        return 0;
    }
    detail.clear();
    bool const after = deskfeed::payload::reg_hook_probe_read(detail);
    deskfeed::log_format(L"probe: hook selftest after install (%s): %s", after ? L"passed" : L"failed",
                         detail.c_str());
    return after ? 1 : 0;
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeBlockVisible() {
    return deskfeed::payload::taskbar_xaml_block_visible() ? 1 : 0;
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeHoverCells(long* out, int capacity) {
    return deskfeed::payload::taskbar_xaml_probe_hover_cells(out, capacity);
}

// 走真实拆除路径。返回 1 = 所有既定门禁都拿到（可以卸载）；0 = 有一条没拿到（必须保留映射）。
// 拆除之后探针开关一律回到正常：变异模式绝不允许跨过拆除留在进程里。
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeTeardown(unsigned timeout_ms) {
    int result = 0;
    try {
        std::wstring detail;
        if (deskfeed::payload::taskbar_xaml_detach(timeout_ms, detail)) {
            result = 1;
        } else {
            deskfeed::log_format(L"probe: teardown did not retire everything: %s", detail.c_str());
        }
    } catch (...) {
        result = 0;
    }
    reset_probe_state();
    return result;
}

extern "C" __declspec(dllexport) void __stdcall DeskFeedProbeStop() {
    std::wstring detail;
    if (!deskfeed::payload::xaml_tap_stop(2000, detail)) {
        deskfeed::log_format(L"probe: diagnostics teardown is incomplete: %s", detail.c_str());
    }
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeDangling() {
    return deskfeed::payload::hook_post_dangling() ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 探针 ABI（共享 POD 定义见 src/payload/probe_api.h；只有自测宿主进程会调用这些入口）。
// 每个入口都自己吞异常：探针是外部线程进来的，异常穿出就是 explorer 终止。
// ---------------------------------------------------------------------------

// R1：把"存好钩子句柄"这一步人为推迟若干毫秒，确定性地制造"回调在安装发布之前进入"。
extern "C" __declspec(dllexport) void __stdcall DeskFeedProbeSetInstallDelay(unsigned ms) {
    try {
        deskfeed::payload::hook_post_set_install_delay_ms(ms);
    } catch (...) {
    }
}

// R2：钩子门的当前状态。只输出 POD 数字，不输出任何指向负载的对象或句柄。
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeHookStatus(
    deskfeed::payload::HookProbeStatus* out, unsigned bytes) {
    if (out == nullptr || bytes < deskfeed::payload::kProbeStatusSize) {
        return 0;
    }
    try {
        deskfeed::payload::HookProbeStatus status{};
        status.size = deskfeed::payload::kProbeStatusSize;
        status.version = deskfeed::payload::kProbeStatusVersion;
        if (!deskfeed::payload::hook_post_probe_status(status)) {
            return 0;
        }
        return copy_status_seh(status, out, deskfeed::payload::kProbeStatusSize) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

// R3：回调阶段记录。`sequence` 让探针分清"看到的是哪一次回调"；`complete` 表示这一次已经走到
// CallNextHookEx 返回之后。**没有**任何"回调已 return"的字段——done 之后不许再观测我们的 return。
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeSetObserver(int enable) {
    try {
        deskfeed::payload::hook_post_probe_set_observer(enable != 0);
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeObserverStage(
    deskfeed::payload::HookProbeStage* out, unsigned bytes) {
    if (out == nullptr || bytes < deskfeed::payload::kProbeStageSize) {
        return 0;
    }
    try {
        deskfeed::payload::HookProbeStage stage{};
        stage.size = deskfeed::payload::kProbeStageSize;
        stage.version = deskfeed::payload::kProbeStageVersion;
        bool const observing = deskfeed::payload::hook_post_probe_observer_stage(stage);
        if (!copy_stage_seh(stage, out, deskfeed::payload::kProbeStageSize)) {
            return 0;
        }
        return observing ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

// R4：完成事件变异。0 = 正常；1 = 提前到 CallNextHookEx 之前（只给对抗用例，生产启动会复位）。
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeSetCompletionMutation(unsigned mode) {
    try {
        if (mode != 0 && mode != 1) {
            return 0;
        }
        deskfeed::payload::hook_post_probe_set_completion_mutation(
            static_cast<deskfeed::payload::CompletionMutation>(mode));
        return 1;
    } catch (...) {
        return 0;
    }
}

// R5：自测宿主自己的托盘窗口。必须属于**本进程**、并且是**同一条** UI 线程（探针随后就用它的
// 线程 id 走真实钩子门）。校验不过就拒绝，绝不把动作投到别的进程/别的线程上。
// 置位后**同时**把探针标记为"已布防"，让这个目标活过随后的真实 `DeskFeedStart`。
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeSetTrayWindow(HWND window) {
    try {
        if (window == nullptr) {
            deskfeed::payload::hook_post_probe_set_tray_window(nullptr);
            deskfeed::payload::hook_post_probe_set_armed(false);
            return 1;
        }
        if (::IsWindow(window) == FALSE) {
            return 0;
        }
        DWORD owner_pid = 0;
        DWORD const owner_thread = ::GetWindowThreadProcessId(window, &owner_pid);
        if (owner_pid != ::GetCurrentProcessId() || owner_thread == 0) {
            return 0;
        }
        deskfeed::payload::hook_post_probe_set_tray_window(window);
        deskfeed::payload::hook_post_probe_set_armed(true);
        return 1;
    } catch (...) {
        return 0;
    }
}
#endif  // DESKFEED_PAYLOAD_PROBE_BUILD

#ifdef DESKFEED_DLL_TEST_API
// ---------------------------------------------------------------------------
// **测试 DLL 专用**入口：只有 `deskfeed-payload-test.dll` 导出，生产 DLL 与原生用例目标里都没有
// 这个符号。它让用例能真的制造"某条回收门在期限内没过"，而不是 mock 一个结果出来。
// ---------------------------------------------------------------------------

// 让动作工作线程每次醒来先真的停住若干毫秒：`action_worker_stop(短超时)` 会真的等不到线程句柄，
// 回收门（`retire_gates.h` 的派生值）因此必须保持 0；解除（传 0）之后重试必须真的等到它退出并给出 1。
extern "C" __declspec(dllexport) int __stdcall DeskFeedTestSetWorkerDelayMs(unsigned ms) {
    try {
        deskfeed::payload::action_worker_probe_set_test_sleep_ms(ms);
        return 1;
    } catch (...) {
        return 0;
    }
}

// 把"工作线程真的起来并进入那一次睡眠"变成可确定的窗口：置好停顿时长，只有当工作线程**已经在跑**
// 时才叫醒它（让它真的睡下去）并返回 1；还没起来就返回 0，调用方据此有界重试。
// 直接调用 `action_worker_probe_set_test_sleep_ms` 而不叫醒，是**不可确定**的：线程可能一直等在
// 唤醒事件上，`stop` 的信号一到，它在 `g_quit` 检查处就退出了，睡眠根本不会发生。
extern "C" __declspec(dllexport) int __stdcall DeskFeedTestStallWorker(unsigned ms) {
    try {
        deskfeed::payload::action_worker_probe_set_test_sleep_ms(ms);
        if (!deskfeed::payload::action_worker_running()) {
            return 0;
        }
        deskfeed::payload::action_worker_post_refresh();
        return 1;
    } catch (...) {
        return 0;
    }
}
#endif  // DESKFEED_DLL_TEST_API

// 状态 ABI（见 src/payload/status_pod.h）：把负载生命周期状态写进调用方在**目标进程里**分配好的
// 定长缓冲。没有指针跨进程传递，也没有字符串两侧传递。`size`/`abi` 不认就写回 `abi = 0`。
extern "C" __declspec(dllexport) DWORD WINAPI DeskFeedGetStatus(void* pod_buffer) {
    try {
        if (pod_buffer == nullptr) {
            return 0;
        }
        auto* const pod = reinterpret_cast<deskfeed::payload::PayloadStatusPod*>(pod_buffer);
        std::uint32_t size = 0;
        std::uint32_t abi = 0;
        if (!read_status_header_seh(pod_buffer, size, abi)) {
            return 0;
        }
        if (size != deskfeed::payload::kPayloadStatusSize ||
            abi != deskfeed::payload::kPayloadStatusAbi) {
            write_status_abi_zero_seh(pod_buffer);  // 调用方据此报"没有状态 ABI"
            return 0;
        }

        deskfeed::payload::PayloadStatusPod status{};
        status.size = deskfeed::payload::kPayloadStatusSize;
        status.abi = deskfeed::payload::kPayloadStatusAbi;
        status.width = deskfeed::payload::kPayloadStatusWidth;
        if (!deskfeed::payload::payload_get_status(status)) {
            return 0;
        }
        return copy_payload_status_seh(status, pod_buffer, deskfeed::payload::kPayloadStatusSize) ? 1
                                                                                                : 0;
    } catch (...) {
        // 这条入口由注入器的 CreateRemoteThread 调用：异常穿出就是 explorer 终止。
        return 0;
    }
}

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        ::DisableThreadLibraryCalls(instance);
        deskfeed::payload::payload_prepare();
        // 进程守卫对象（租约 mutex / 粘性不确定 event / 入口记录 mapping）在**加载器锁**里一次性
        // 拿到引用：只 kernel32 + 栈上定长缓冲，不等待、不分配。失败只让 Stop 永久 fail closed，
        // 不影响加载语义。契约见 payload/guard_record.h 与 docs/native-safety.md。
        deskfeed::payload::payload_guard_prepare();
    } else if (reason == DLL_PROCESS_DETACH) {
        // 只归还本模块自己那 3 个引用 + 只读视图，**不等待**、不分配。healthy actor 自己持着这些
        // 命名对象跨过 `FreeLibrary`，所以记录不会因为这里归还而消失。
        deskfeed::payload::payload_guard_release();
    }
    return TRUE;
}
