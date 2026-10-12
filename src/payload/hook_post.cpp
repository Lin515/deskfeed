#include "payload/hook_post.h"

#include "platform/log.h"
#include "platform/win32_util.h"

#include <atomic>
#include <mutex>
#include <string>

// 实现顺序本身就是证明的一部分（详见头注释与 docs/native-safety.md 的"任务栏线程编组"）：
//
//   T = 任务栏那一条 UI 线程，M = 本模块。
//   钩子过程 p：
//     ① 取记录：`exchange(nullptr)`；取不到且确实有一次待跑的动作时，**有界地**等一次安装发布，
//        再取一次；取到就自己 `UnhookWindowsHookEx`（这是唯一的静止构造）。
//     ② `in_flight++`。
//     ③ 只有在"记录是自己摘掉的"时候才跑执行体：执行体与**结果发布**都在 C++/SEH 边界里，且
//        回调里不做日志、不做会分配的事。
//     ④ `in_flight--`。
//     ⑤ `LRESULT r = CallNextHookEx(...)`（排在我们后面的第三方钩子**可能泵消息**）。
//     ⑥ `SetEvent(done)`：完成事件，手动复位，只有摘掉记录的那次回调才置位。
//     ⑦ 裸 `return r`：后面不得再有析构、日志、user32/kernel 调用或异常处理器。
//
//   收尾线程（不是 T）：**先等完成事件**（钩子句柄为空不算完成），再向"当前实际的、属于同一个 T
//   与同一个进程的任务栏窗口"发一次非排队消息；返回成功才归还非 PIN 自引用。
namespace deskfeed::payload {
namespace {

struct Gate {
    // 串行化"投递"与"收尾"。钩子过程**绝不**取它，否则收尾时发 ping 就是死锁而不是证明。
    std::mutex run_mutex;

    std::atomic<HHOOK> hook{nullptr};
    HANDLE done = nullptr;       // 手动复位：回调最后一步置位；安装前复位
    HANDLE published = nullptr;  // 手动复位：安装方存好句柄后置位（安装/发布握手）

    std::atomic<unsigned> in_flight{0};
    std::atomic<bool> retired{false};
    std::atomic<bool> still_installed{false};
    std::atomic<bool> pending{false};
    // 有一次安装还没拿到"完成 + ping"这两个证明。收尾只看它，不看句柄是否为空。
    std::atomic<bool> completion_expected{false};

    HookAction action = HookAction::Apply;
    bool action_ok = false;
    // 预留了容量：回调里的赋值不会分配。
    std::wstring action_error;

    // SEH 故障证据。过滤器只做无分配的记录，日志留给调用方。
    std::atomic<unsigned long> fault_code{0};
    std::atomic<unsigned long long> fault_offset{0};

    std::atomic<bool> dangling{false};
    HMODULE self_ref = nullptr;

    HookExecutor executor = nullptr;
    void* context = nullptr;

    unsigned install_delay_ms = 0;  // 自测用的确定性对抗窗口
};

Gate g;

// SEH 记录体：**不分配、不日志**（日志可能在故障路径上抛出，而这里已经有一次异常了）。
int fault_filter(EXCEPTION_POINTERS* info) {
    unsigned long code = 0;
    void* address = nullptr;
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        code = static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode);
        address = info->ExceptionRecord->ExceptionAddress;
    }
    unsigned long long offset = 0;
    HMODULE module = nullptr;
    if (address != nullptr &&
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(address), &module) != FALSE) {
        offset = static_cast<unsigned long long>(reinterpret_cast<unsigned char*>(address) -
                                                 reinterpret_cast<unsigned char*>(module));
    }
    g.fault_code.store(code);
    g.fault_offset.store(offset);
    return EXCEPTION_EXECUTE_HANDLER;
}

// SEH 边界。只允许引用参数：这个函数不能有需要展开的局部对象。
bool run_executor_seh(HookAction action, std::wstring& error, bool& ok) {
    __try {
        ok = g.executor != nullptr && g.executor(g.context, action, error);
    } __except (fault_filter(GetExceptionInformation())) {
        ok = false;
    }
    return true;
}

// 参考实现里那个"系统中立窗口"的取法：探针宿主没有 Shell_TrayWnd，就由宿主自己给一个
// user32 类的窗口（STATIC），绝不能用本模块自己的窗口过程当 ack 目标——那会引入新的 `ret` 问题。
HWND g_ack_window_override = nullptr;

HWND resolve_ack_window(HWND preferred) {
    if (preferred != nullptr && ::IsWindow(preferred) != FALSE) {
        return preferred;
    }
    if (g_ack_window_override != nullptr && ::IsWindow(g_ack_window_override) != FALSE) {
        return g_ack_window_override;
    }
    HWND window = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (window == nullptr) {
        window = ::FindWindowW(L"Shell_SecondaryTrayWnd", nullptr);
    }
    return window;
}

// 装钩前取一次非 PIN 自引用。取不到就不能装：没有它，"user32 已经决定调用、还没进入"的那一瞬间
// 模块可能已经被卸载。
bool acquire_self_reference(std::wstring& error) {
    if (g.self_ref != nullptr) {
        return true;
    }
    HMODULE module = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                             reinterpret_cast<LPCWSTR>(&acquire_self_reference), &module) == FALSE) {
        error = L"取本模块的加载器引用失败: " + last_error_text();
        return false;
    }
    g.self_ref = module;
    return true;
}

void release_self_reference() {
    if (g.self_ref != nullptr) {
        ::FreeLibrary(g.self_ref);
        g.self_ref = nullptr;
    }
}

bool ensure_events(std::wstring& error) {
    if (g.done == nullptr) {
        g.done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);  // 手动复位
        if (g.done == nullptr) {
            error = L"建立钩子完成事件失败: " + last_error_text();
            return false;
        }
    }
    if (g.published == nullptr) {
        g.published = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g.published == nullptr) {
            error = L"建立钩子发布事件失败: " + last_error_text();
            return false;
        }
    }
    return true;
}

}  // namespace

void hook_post_set_executor(HookExecutor executor, void* context) {
    std::lock_guard<std::mutex> guard(g.run_mutex);
    g.executor = executor;
    g.context = context;
}

LRESULT CALLBACK hook_proc(int code, WPARAM wparam, LPARAM lparam);

namespace {

LRESULT CALLBACK hook_proc_impl(int code, WPARAM wparam, LPARAM lparam) {
    LRESULT next = 0;
    bool signal_done = false;
    {
        // ① 取记录并自摘。取不到而确实有一次待跑的动作，说明安装方还没把句柄发布出来（T 可以在
        //    SetWindowsHookExW 返回与 store 之间就进入回调）——有界地等一次发布事件再取。
        HHOOK installed = g.hook.exchange(nullptr);
        if (installed == nullptr && g.pending.load(std::memory_order_acquire)) {
            ::WaitForSingleObject(g.published, 200);
            installed = g.hook.exchange(nullptr);
        }

        bool removed = false;
        if (installed != nullptr) {
            removed = ::UnhookWindowsHookEx(installed) != FALSE;
            if (removed) {
                g.retired.store(true);
                signal_done = true;
            } else {
                // 记录可能还在链上：本次绝不声明已退休，也绝不跑动作。
                g.still_installed.store(true);
            }
        }

        // ②
        g.in_flight.fetch_add(1, std::memory_order_acq_rel);

        // ③ 只有摘掉记录的那次回调才跑动作；执行体与结果发布都在 C++/SEH 边界内。
        if (removed && g.pending.exchange(false)) {
            bool ok = false;
            try {
                g.fault_code.store(0);
                g.fault_offset.store(0);
                run_executor_seh(g.action, g.action_error, ok);
            } catch (...) {
                // 执行体或结果发布抛出的任何东西都在这里吃掉：绝不能穿过 OS 的钩子 ABI。
                ok = false;
            }
            g.action_ok = ok;
        }

        // ④
        g.in_flight.fetch_sub(1, std::memory_order_acq_rel);

        // ⑤ 第三方钩子可能在这里泵消息：到此为止一切都必须做完。
        next = ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    // ⑥ 完成事件是最后一条模块 API；⑦ 之后是裸返回。
    if (signal_done) {
        ::SetEvent(g.done);
    }
    return next;
}

}  // namespace

LRESULT CALLBACK hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code < 0) {
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }
    return hook_proc_impl(code, wparam, lparam);
}

HookPostResult hook_post_run(DWORD tray_thread_id, HookAction action, unsigned timeout_ms) {
    std::lock_guard<std::mutex> guard(g.run_mutex);
    HookPostResult result;

    if (!ensure_events(result.error)) {
        return result;
    }

    // 上一轮还没有拿到"完成"：只等它，绝不装第二个记录，也绝不从这边强摘。
    if (g.completion_expected.load(std::memory_order_acquire)) {
        if (::WaitForSingleObject(g.done, timeout_ms) != WAIT_OBJECT_0) {
            g.dangling.store(true);
            result.armed = g.hook.load() != nullptr;
            result.error = L"任务栏线程还没有跑完上一轮动作";
            return result;
        }
    }
    if (g.still_installed.load()) {
        g.dangling.store(true);
        result.error = L"钩子记录没有被摘掉（UnhookWindowsHookEx 在回调里失败过）";
        return result;
    }
    if (tray_thread_id == 0) {
        result.error = L"没有可用的任务栏线程";
        return result;
    }

    // 自引用必须在装钩**之前**取。
    if (!acquire_self_reference(result.error)) {
        return result;
    }

    g.retired.store(false);
    g.still_installed.store(false);
    g.pending.store(true);
    g.action = action;
    g.action_ok = false;
    g.action_error.clear();
    g.action_error.reserve(256);  // 回调里的赋值不分配
    g.fault_code.store(0);
    g.fault_offset.store(0);
    ::ResetEvent(g.done);
    ::ResetEvent(g.published);
    g.completion_expected.store(true);

    // 同进程线程钩子：hMod 必须是 NULL。
    HHOOK const installed = ::SetWindowsHookExW(WH_GETMESSAGE, hook_proc, nullptr, tray_thread_id);
    if (installed == nullptr) {
        std::wstring const reason = last_error_text();
        g.pending.store(false);
        g.completion_expected.store(false);
        // 安装失败：立刻把刚才取的自引用还回去，引用数回到调用方那一份。
        release_self_reference();
        result.error = L"SetWindowsHookEx(WH_GETMESSAGE) 失败: " + reason;
        return result;
    }

    // 自测用：把"发布句柄"这一步人为推迟，用来确定性地制造"回调先于发布进入"。
    if (g.install_delay_ms != 0) {
        ::Sleep(g.install_delay_ms);
    }

    g.hook.store(installed);
    ::SetEvent(g.published);  // 安装/发布握手：回调取不到句柄时等它
    result.armed = true;

    // 钩子只在线程取消息时才有机会跑，所以要把它叫醒。消息本身无关紧要。
    bool posted = false;
    for (int attempt = 0; attempt < 40 && !posted; ++attempt) {
        posted = ::PostThreadMessageW(tray_thread_id, WM_NULL, 0, 0) != FALSE;
        if (!posted) {
            ::Sleep(25);
        }
    }
    if (!posted) {
        // 不在这里强摘：记录留在链上，T 下一次取消息时会自摘并跑动作。
        g.dangling.store(true);
        result.error = L"向任务栏线程投递唤醒消息失败: " + last_error_text();
        return result;
    }

    if (::WaitForSingleObject(g.done, timeout_ms) != WAIT_OBJECT_0) {
        g.dangling.store(true);
        result.error = L"任务栏线程在 " + std::to_wstring(timeout_ms) + L" ms 内没有完成动作";
        return result;
    }

    result.ok = g.action_ok;
    result.retired = g.retired.load();
    if (!result.ok) {
        if (g.fault_code.load() != 0) {
            result.error = L"执行任务栏动作时捕获到硬件故障 0x" + hex_text(g.fault_code.load()) +
                           L"（相对本模块 +0x" + hex_text(g.fault_offset.load()) + L"）";
        } else {
            result.error = g.action_error.empty() ? L"任务栏动作失败" : g.action_error;
        }
    }
    return result;
}

bool hook_post_retire(HWND ack_window, DWORD tray_thread_id, unsigned timeout_ms,
                      std::wstring& detail) {
    std::lock_guard<std::mutex> guard(g.run_mutex);

    if (!g.completion_expected.load(std::memory_order_acquire)) {
        // 没有待证实的安装。悬挂记录不该在这种状态下存在；有就如实报告未退休。
        if (g.dangling.load()) {
            detail = L"还有未证明安全的悬挂";
            return false;
        }
        return true;
    }

    // **完成事件是唯一的门**：句柄为空只说明回调开头取走了它，执行体、CallNextHookEx 与它们的
    // 析构可能都还没跑完。手动复位事件在最后一步置位，所以这里必须等它本身。
    if (::WaitForSingleObject(g.done, timeout_ms) != WAIT_OBJECT_0) {
        g.dangling.store(true);
        detail = L"任务栏线程还没有完成动作（钩子记录可能仍在链上）";
        return false;
    }
    if (!g.retired.load() || g.still_installed.load()) {
        g.dangling.store(true);
        detail = L"钩子记录没有在回调里被摘掉";
        return false;
    }
    if (g.in_flight.load(std::memory_order_acquire) != 0) {
        g.dangling.store(true);
        detail = L"完成事件之后仍有回调在途";
        return false;
    }

    // ping：跨线程的非排队消息。它只可能在 T 回到消息检索路径、执行完一次窗口过程之后返回，
    // 而钩子过程是 GetMessage/PeekMessage 调用栈里的内层帧 ⇒ 这次成功返回就证明帧已经弹掉。
    HWND const window = resolve_ack_window(ack_window);
    if (window == nullptr) {
        g.dangling.store(true);
        detail = L"找不到属于任务栏线程的确认窗口，无法证明钩子帧已弹掉";
        return false;
    }
    DWORD owner_pid = 0;
    DWORD const owner_thread = ::GetWindowThreadProcessId(window, &owner_pid);
    if (tray_thread_id != 0 && owner_thread != tray_thread_id) {
        g.dangling.store(true);
        detail = L"确认窗口的属主线程不是那个任务栏线程（TID " + std::to_wstring(owner_thread) + L"）";
        return false;
    }
    if (owner_pid != ::GetCurrentProcessId()) {
        g.dangling.store(true);
        detail = L"确认窗口属于别的进程（PID " + std::to_wstring(owner_pid) + L"）";
        return false;
    }

    DWORD_PTR ignored = 0;
    if (::SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_BLOCK | SMTO_ABORTIFHUNG, 2000,
                              &ignored) == 0) {
        g.dangling.store(true);
        detail = L"确认窗口在 2000 ms 内没有应答，钩子帧是否弹掉未证明";
        return false;
    }

    // ping 之后再核验一次身份：这段时间里窗口没有换主人。
    DWORD after_pid = 0;
    DWORD const after_thread = ::GetWindowThreadProcessId(window, &after_pid);
    if (after_thread != owner_thread || after_pid != owner_pid) {
        g.dangling.store(true);
        detail = L"ping 期间确认窗口的主人变了";
        return false;
    }

    // 证明齐了，归还非 PIN 自引用。
    release_self_reference();
    g.completion_expected.store(false);
    g.dangling.store(false);
    return true;
}

bool hook_post_dangling() { return g.dangling.load(); }

bool hook_post_armed() { return g.hook.load() != nullptr; }

void hook_post_set_ack_window(HWND window) { g_ack_window_override = window; }

void hook_post_set_install_delay_ms(unsigned delay_ms) {
    std::lock_guard<std::mutex> guard(g.run_mutex);
    g.install_delay_ms = delay_ms;
}

}  // namespace deskfeed::payload
