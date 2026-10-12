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
//
//   三段证明的先后关系必须记准（不是"事件之后才返回"）：
//     sentinel（第三方在 CallNext 内层帧里的观测/pump 返回）
//       < done（我们的 LAST SetEvent）
//       < 我们的裸 return 真正完成、钩子帧弹掉
//       < UI 取到并处理完那次 WM_NULL
//       < SendMessageTimeout 返回 ack
//       < FreeLibrary 归还自引用。
//   ack 的成功恰恰**证明**我们的 return 已经过去；如果 ack 能在我们的 return 之前返回，说明
//   消息泵进了我们自己的钩子帧，那时必须拒绝卸载（`hook_post_retire` 的不返回/失败路径）。
//   `done` 之后不得再用任何日志或 API 去观测 return——观测点只在模块之外（探针在 host 侧看）。
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

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
    unsigned install_delay_ms = 0;  // 自测用的确定性对抗窗口

    // 探针专用（见 src/payload/probe_api.h）。这些量只在自测构建里存在；生产 DLL 里连它们
    // 带的那几条代码路径都不编译。
    std::atomic<bool> observer_enabled{false};
    std::atomic<unsigned> observer_stage{0};
    std::atomic<unsigned> observer_sequence{0};
    std::atomic<unsigned> completion_mutation{0};
    // 探针"已布防"：见 hook_post_probe_set_armed 的注释。
    std::atomic<bool> probe_armed{false};
    // **粘性**：这一次安装真的走过了"完成事件提前置位"的变异路径。只要它还在，收尾一律按"未退休"
    // 处理，绝不归还自引用——提前置位的完成事件证明不了我们那一帧已经从 T 的栈上弹掉。
    std::atomic<bool> completion_mutated{false};
#endif
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

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
// 探针的调用阶段记录。只写两个原子量，不分配、不日志、不碰任何句柄——下面这几步全部发生在
// **最后一次 SetEvent 之前**，所以对回调末尾"只剩一次 SetEvent 与裸 return"这条断言没有影响。
// `stage` 只增不减；`sequence` 每次回调进入时自增一次，探针据此区分"看到的是哪一次回调"。
void observer_note(unsigned stage) {
    if (!g.observer_enabled.load(std::memory_order_acquire)) {
        return;
    }
    g.observer_stage.store(stage, std::memory_order_release);
}
#define DESKFEED_OBSERVE(stage) observer_note(static_cast<unsigned>(stage))
#else
// 生产构建：观测点**不存在**。参数里的探针枚举也随之整段消失（宏不解析被丢弃的实参）。
#define DESKFEED_OBSERVE(stage) ((void)0)
#endif

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
#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
        if (g.observer_enabled.load(std::memory_order_acquire)) {
            g.observer_sequence.fetch_add(1, std::memory_order_acq_rel);
        }
#endif
        DESKFEED_OBSERVE(HookObserverStage::Enter);
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
        DESKFEED_OBSERVE(HookObserverStage::RecordRemoved);

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

        // 探针阶段：执行体与结果发布都跑完了（仍然在最后一处 SetEvent 之前）。
        DESKFEED_OBSERVE(HookObserverStage::ExecutorDone);

        // ④
        g.in_flight.fetch_sub(1, std::memory_order_acq_rel);

        // ⑤ 第三方钩子可能在这里泵消息：到此为止一切都必须做完。
        DESKFEED_OBSERVE(HookObserverStage::CallNextEntered);
        next = ::CallNextHookEx(nullptr, code, wparam, lparam);
        DESKFEED_OBSERVE(HookObserverStage::CallNextReturned);

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
        // 探针的对抗开关：把完成事件故意提前到 CallNextHookEx 之前。正常路径下事件只在下面是
        // **最后一次**模块 API；变异路径下它落在 CallNext 之前，用来证明验收能抓住这类错误。
        // 一旦真的走过这条路，就置**粘性**的 `completion_mutated`：收尾那边见到它就一律按"未退休"
        // 处理、不归还自引用（提前置位的事件证明不了我们那一帧已经从 T 的栈上弹掉）。
        if (g.completion_mutation.load(std::memory_order_acquire) ==
            static_cast<unsigned>(CompletionMutation::EarlyBeforeCallNext)) {
            if (g.done != nullptr) {
                ::SetEvent(g.done);
                signal_done = false;
                g.completion_mutated.store(true, std::memory_order_release);
            }
        }
#endif
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

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
    // 自测用：把"发布句柄"这一步人为推迟，用来确定性地制造"回调先于发布进入"。生产里没有它。
    if (g.install_delay_ms != 0) {
        ::Sleep(g.install_delay_ms);
    }
#endif

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

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
    // 变异路径的观测到此为止：上面的等待、身份核验与 ping **都已经真的执行过**（第三方钩子在
    // CallNextHookEx 里泵消息、ACK 落在我们的 return 之前时，这次 ping 的结果就是那条反例证据），
    // 但结论**强制**为"未退休"——提前置位的完成事件不能证明我们那一帧已经弹掉，所以绝不归还自引用。
    // 调用方要拿正常证据，必须先确认回调真的已经返回、把变异关回 Normal，再走一次本函数。
    if (g.completion_mutated.load(std::memory_order_acquire)) {
        g.dangling.store(true);
        detail = L"完成事件被变异提前置位（对抗用例）：本轮一律按未退休处理，不归还自引用";
        return false;
    }
#endif

    // 证明齐了，归还非 PIN 自引用。
    release_self_reference();
    g.completion_expected.store(false);
    g.dangling.store(false);
    return true;
}

bool hook_post_dangling() { return g.dangling.load(); }

bool hook_post_armed() { return g.hook.load() != nullptr; }

// `DllMain(DLL_PROCESS_DETACH)` 调用。条件与 `hook_post_retire` 的成功结论同构（见头文件注释）：
// `completion_expected` 为假只可能来自"从来没装成过"或"收尾两道证明都拿到"，再叠上同一份状态机上
// 的四条冗余印证（记录不在链上、没有在途回调、没有悬挂、没有待跑动作）。任何一条不成立就一个都不
// 关：宁可让进程在退出前多持两个句柄，也绝不把回调还要用的事件提前关掉。
//
// 不取 `g.run_mutex`：DETACH 在加载器锁里，而这里读的全是原子量 + 两个只有本线程会写的 HANDLE 槽
// 位；合作 actor 的纪律保证了此时没有投递/收尾在飞（否则 Stop 不会返回 1、actor 也不会 FreeLibrary）。
void hook_post_release_owned_handles() noexcept {
    bool const fully_retired = !g.completion_expected.load(std::memory_order_acquire) &&
                               !g.dangling.load(std::memory_order_acquire) &&
                               !g.pending.load(std::memory_order_acquire) &&
                               !g.still_installed.load(std::memory_order_acquire) &&
                               g.hook.load(std::memory_order_acquire) == nullptr &&
                               g.in_flight.load(std::memory_order_acquire) == 0;
    if (!fully_retired) {
        return;
    }
    if (g.done != nullptr) {
        ::CloseHandle(g.done);
        g.done = nullptr;
    }
    if (g.published != nullptr) {
        ::CloseHandle(g.published);
        g.published = nullptr;
    }
}

void hook_post_set_ack_window(HWND window) { g_ack_window_override = window; }

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
// ---------------------------------------------------------------------------
// 探针 ABI 的内部实现（导出的入口在 dllmain.cpp 里做 POD 校验与受控填充）。整段只在自测构建里
// 编译：生产 DLL 里没有 observer、没有变异开关、没有安装延迟，也没有"已布防"。
// ---------------------------------------------------------------------------

void hook_post_set_install_delay_ms(unsigned delay_ms) {
    std::lock_guard<std::mutex> guard(g.run_mutex);
    g.install_delay_ms = delay_ms;
}

bool hook_post_probe_status(HookProbeStatus& out) {
    out.hook_installed = g.hook.load() != nullptr ? 1u : 0u;
    out.retired = g.retired.load() ? 1u : 0u;
    out.still_installed = g.still_installed.load() ? 1u : 0u;
    out.in_flight = g.in_flight.load(std::memory_order_acquire);
    out.pending = g.pending.load(std::memory_order_acquire) ? 1u : 0u;
    out.completion_expected = g.completion_expected.load(std::memory_order_acquire) ? 1u : 0u;
    out.dangling = g.dangling.load() ? 1u : 0u;
    // 只**观测**完成事件当前是否置位：它绝不能反过来当成收尾门（收尾门只认 `completion_expected`
    // 与那次等待本身）。
    out.done_signaled =
        (g.done != nullptr && ::WaitForSingleObject(g.done, 0) == WAIT_OBJECT_0) ? 1u : 0u;
    return true;
}

bool hook_post_probe_observer_stage(HookProbeStage& out) {
    out.stage = g.observer_stage.load(std::memory_order_acquire);
    out.sequence = g.observer_sequence.load(std::memory_order_acquire);
    out.complete = out.stage >= static_cast<unsigned>(HookObserverStage::CallNextReturned)
                       ? 1u
                       : 0u;
    return g.observer_enabled.load(std::memory_order_acquire);
}

void hook_post_probe_set_observer(bool enable) {
    if (enable) {
        g.observer_stage.store(static_cast<unsigned>(HookObserverStage::None),
                               std::memory_order_release);
        g.observer_sequence.store(0, std::memory_order_release);
        g.observer_enabled.store(true, std::memory_order_release);
        return;
    }
    g.observer_enabled.store(false, std::memory_order_release);
    g.observer_stage.store(static_cast<unsigned>(HookObserverStage::None),
                           std::memory_order_release);
    g.observer_sequence.store(0, std::memory_order_release);
}

void hook_post_probe_set_completion_mutation(CompletionMutation mode) {
    g.completion_mutation.store(static_cast<unsigned>(mode), std::memory_order_release);
    if (mode == CompletionMutation::Normal) {
        // 只有确认回调真的已经返回之后才允许关回 Normal；关回去之后，收尾才可能给出"可以卸载"的
        // 结论（届时完成事件、retired、in_flight 与 ping 都会按正常路径重新核一遍）。
        g.completion_mutated.store(false, std::memory_order_release);
    }
}

// 自测宿主"已布防"：R5 指定的托盘窗口必须活过 `DeskFeedStart`（否则真实 mailbox 的 UI 投递又会
// 回到 `FindWindow(Shell_TrayWnd)`，探针场景根本跑不起来）。只有探针显式置位才生效。
bool hook_post_probe_armed() { return g.probe_armed.load(std::memory_order_acquire); }

void hook_post_probe_set_armed(bool armed) {
    g.probe_armed.store(armed, std::memory_order_release);
}

void hook_post_probe_set_tray_window(HWND window) { g_ack_window_override = window; }
#endif  // DESKFEED_PAYLOAD_PROBE_BUILD

}  // namespace deskfeed::payload
