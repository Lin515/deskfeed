#include "payload/host.h"

#include "core/utf8.h"
#include "display/frame.h"
#include "display/mailbox.h"
#include "display/reading.h"
#include "payload/action_worker.h"
#include "payload/feed.h"
#include "payload/hook_post.h"
#include "payload/taskbar_xaml.h"
#include "payload/xaml_tap.h"
#include "platform/log.h"
#include "platform/paths.h"
#include "platform/win32_util.h"

#include <windows.h>

// 当前线程上建调度队列要用原始入口：SDK 的 C++/WinRT 投影只暴露 CreateOnDedicatedThread。
#include <dispatcherqueue.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>

#include <atomic>
#include <exception>
#include <string>

namespace deskfeed::payload {
namespace {

std::atomic<bool> g_stop_requested{false};
DWORD g_thread_id = 0;
HANDLE g_cleanup_event = nullptr;
HANDLE g_payload_thread = nullptr;
std::atomic<bool> g_thread_published{false};
std::atomic<DWORD> g_unload_safe{0};
// 状态 ABI（见 src/payload/status_pod.h）要的三件事：邮箱是否 attach 成功、最近一轮的连续坏帧
// 轮数、以及负载线程当前正在消费的那个消费者。都只是**观测**值。
std::atomic<bool> g_mailbox_attached{false};
std::atomic<unsigned> g_feed_failures{0};
std::atomic<DisplayFrameConsumer*> g_consumer{nullptr};
winrt::Windows::System::DispatcherQueueController g_dispatcher_controller{nullptr};

bool create_dispatcher_queue() {
    DispatcherQueueOptions options{};
    options.dwSize = sizeof(options);
    options.threadType = DQTYPE_THREAD_CURRENT;
    options.apartmentType = DQTAT_COM_NONE;

    winrt::com_ptr<ABI::Windows::System::IDispatcherQueueController> raw;
    HRESULT const result = ::CreateDispatcherQueueController(
        options, reinterpret_cast<PDISPATCHERQUEUECONTROLLER*>(raw.put()));
    if (FAILED(result)) {
        log_format(L"CreateDispatcherQueueController -> %s", hresult_text(result).c_str());
        return false;
    }
    g_dispatcher_controller =
        winrt::Windows::System::DispatcherQueueController{raw.detach(), winrt::take_ownership_from_abi};
    log_line(L"dispatcher queue created on current thread");
    return true;
}

void run_message_loop(DisplayFrameConsumer const& consumer) {
    HANDLE wait_handles[2] = {consumer.ready_event(), consumer.producer_process()};
    MSG message{};
    while (!g_stop_requested.load()) {
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                return;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        // 用 MsgWaitForMultipleObjects 让出（而不是阻塞在 GetMessage）：本线程的调度队列才有机会
        // 跑它的工作项，XAML 的 composition 提交就是这样送出去的。
        DWORD const wait = ::MsgWaitForMultipleObjectsEx(2, wait_handles, 250, QS_ALLINPUT,
                                                         MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0 + 1) {
            // 生产者进程句柄触发 = 主进程已经退出（正常或崩溃）。没有来源的读数必须立刻消失。
            log_line(L"the display mailbox producer is gone; removing the taskbar block");
            break;
        }
        // ready 事件（WAIT_OBJECT_0）与超时都在下面这一轮里处理：统一读一次邮箱。
        break;
    }
}

// 收尾日志与清理事件都可能在"已经有一次异常在路上"的时候才跑，所以每一步都要自己吞掉异常：
// 不能因为记账失败就把进程带走。
void safe_log_line(wchar_t const* text) {
    try {
        log_line(text);
    } catch (...) {
    }
}

void safe_log_wide(wchar_t const* prefix, std::wstring const& text) {
    try {
        log_format(L"%s%s", prefix, text.c_str());
    } catch (...) {
    }
}

void safe_finalize(DWORD unload_safe) {
    try {
        if (hook_post_dangling()) {
            g_unload_safe.store(0);
            log_line(L"a hook is still dangling; the module must stay loaded");
        } else {
            g_unload_safe.store(unload_safe);
        }
        log_format(L"payload stopped (unload safe: %s)",
                   g_unload_safe.load() == 1 ? L"yes" : L"no");
    } catch (...) {
        // 记账失败时按"未退休"处理：宁可多留一份映射，也不能报告可卸载。
        g_unload_safe.store(0);
    }

    // 调度队列刻意**不**经 ShutdownQueueAsync 关：它的完成回调会回到本模块，上一次实测直接把
    // explorer 打崩。丢掉控制器就够了，队列随线程消失。
    try {
        g_dispatcher_controller = nullptr;
    } catch (...) {
    }

    if (g_cleanup_event != nullptr) {
        // 清理事件只是**观测**信号，不是卸载门；在模块代码全部退出之前置位。
        ::SetEvent(g_cleanup_event);
    }
    try {
        log_close();
    } catch (...) {
    }
}

// SEH 边界的故障记录。只允许引用参数，不能有需要展开的局部对象；这里只写两个 out 参数，
// `GetModuleHandleExW` 不分配，日志留给调用方。
void record_fault(EXCEPTION_POINTERS* info, unsigned long& code, unsigned long long& offset) {
    code = 0;
    offset = 0;
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        return;
    }
    code = static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode);
    void* const address = info->ExceptionRecord->ExceptionAddress;
    HMODULE module = nullptr;
    if (address != nullptr &&
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(address), &module) != FALSE) {
        offset = static_cast<unsigned long long>(reinterpret_cast<unsigned char*>(address) -
                                                 reinterpret_cast<unsigned char*>(module));
    }
}

// SEH 一层：在 C++ 对象全部析构之后进入，只调用一个不在本函数里展开任何东西的函数指针。
void run_core_seh(void (*core)(HINSTANCE, std::wstring const&), HINSTANCE module_instance,
                  std::wstring const& scope, unsigned long& fault_code,
                  unsigned long long& fault_offset) {
    __try {
        core(module_instance, scope);
    } __except (record_fault(GetExceptionInformation(), fault_code, fault_offset),
                EXCEPTION_EXECUTE_HANDLER) {
    }
}

// 负载线程真正做的事。收尾（清理事件、卸载门、日志收尾）由外层 `payload_run` 在边界之外负责，
// 所以这里可以**提前返回**，任何一条路径都不会漏掉收尾。
void payload_run_core(HINSTANCE module_instance, std::wstring const& scope) {
    (void)module_instance;

    // 与 explorer 的 DPI 感知对齐，免得子窗口被虚拟化到错误的像素上。
    ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 调度队列必须先于 WinRT 单元：compositor 绑定创建时当前线程的那个队列。
    create_dispatcher_queue();
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (winrt::hresult_error const& failure) {
        log_format(L"init_apartment failed: %s", hresult_text(failure.code()).c_str());
    }

    std::string attach_error;
    DisplayFrameConsumer consumer;
    if (!consumer.attach(scope, attach_error)) {
        // 生产者已经不在了也算失败：据此立刻清理，不留旧读数。
        log_format(L"display mailbox: not attached: %S", attach_error.c_str());
        return;
    }
    g_mailbox_attached.store(true);
    g_consumer.store(&consumer);
    log_line(L"display mailbox attached");

    // 第一份读数：主进程启动时就会发一帧，所以任务栏块一出现就是真实内容，不是占位。
    Frame first;
    if (consumer.read(first)) {
        taskbar_xaml_probe_set_reading(reading_from_frame(first));
    } else {
        taskbar_xaml_probe_set_reading(reading_from_frame(Frame{}));
    }

    std::wstring worker_error;
    if (!action_worker_start(scope, worker_error)) {
        log_format(L"action worker: %s", worker_error.c_str());
    }

    std::wstring attach_xaml_error;
    if (taskbar_xaml_attach(scope, attach_xaml_error)) {
        log_line(L"taskbar reading block applied");
    } else {
        log_format(L"taskbar xaml attach failed: %s", attach_xaml_error.c_str());
    }

    unsigned failures = 0;
    bool have_last = consumer.read(first);
    while (!g_stop_requested.load()) {
        run_message_loop(consumer);
        if (g_stop_requested.load()) {
            break;
        }
        if (!consumer.producer_running()) {
            // 生产者已死：拆块，不保留旧读数。
            log_line(L"the display mailbox producer died; the reading has no source any more");
            taskbar_xaml_request_update(ReadingContent{});
            break;
        }
        Frame frame;
        bool const read_ok = consumer.read(frame);
        FeedInput input;
        input.read_ok = read_ok;
        input.closed = read_ok && frame.closed;
        input.owner_alive = consumer.producer_running();
        input.have_last_frame = have_last;
        input.failures = failures;
        FeedOutcome const decision = feed_decide(input);
        if (decision == FeedOutcome::Apply) {
            have_last = true;
            failures = 0;
            g_feed_failures.store(0);
            taskbar_xaml_request_update(reading_from_frame(frame));
        } else if (decision == FeedOutcome::Keep) {
            ++failures;
            g_feed_failures.store(failures);
        } else {
            // Remove 有三种不同原因，日志必须分得开：验收要能看出"生产者正常收尾"、"生产者没了"与
            // "帧坏了又没有可沿用的旧帧"。
            failures = 0;
            have_last = false;
            g_feed_failures.store(0);
            if (input.closed) {
                log_line(L"the display mailbox closed by the producer; removing the taskbar block");
            } else if (!input.owner_alive) {
                log_line(L"the producer is gone; dropping the reading and removing the taskbar block");
            } else {
                log_line(L"the reading is dropped (no complete frame and no usable last frame)");
            }
            taskbar_xaml_request_update(ReadingContent{});
            break;
        }
    }

    // 拆除顺序：动作工作线程 → 任务栏块与悬停/菜单 → 钩子的静止证明。任何一步没有拿到
    // "已终止/已证明"都算未退休，绝不能报告可卸载。
    if (!action_worker_stop(4000)) {
        g_unload_safe.store(0);
    }
    std::wstring detach_detail;
    if (!taskbar_xaml_detach(4000, detach_detail)) {
        g_unload_safe.store(0);
        log_format(L"teardown did not retire everything: %s", detach_detail.c_str());
    }
    xaml_tap_stop();
    g_consumer.store(nullptr);
    g_mailbox_attached.store(false);
    consumer.detach();
}

}  // namespace

void payload_prepare() {
    if (g_cleanup_event == nullptr) {
        std::wstring const name = L"Local\\DeskFeed-payload-cleanup-" +
                                  std::to_wstring(::GetCurrentProcessId());
        g_cleanup_event = ::CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        if (g_cleanup_event != nullptr &&
            ::WaitForSingleObject(g_cleanup_event, 0) == WAIT_OBJECT_0) {
            // 名字虽然带 pid，仍然是会话级对象：万一同名对象已经存在且已置位（上一次实验留下的
            // 句柄还没关掉），这一次的 stop 会立刻返回。开跑前清一次，别让交接错位。
            log_line(L"warning: the cleanup event was already signalled; resetting it");
            ::ResetEvent(g_cleanup_event);
        }
    }
}

HANDLE payload_cleanup_event() { return g_cleanup_event; }

HANDLE payload_thread_handle() { return g_payload_thread; }

// 由 `DeskFeedStart` 在**线程开始跑之前**登记它刚创建的线程句柄（配合 `CREATE_SUSPENDED`）。
// 这条登记是"立即 DeskFeedStop"能拿到真实句柄的唯一保证：以前句柄是在负载线程里
// `DuplicateHandle` 出来的，线程还没被调度时 `payload_thread_handle()` 返回 nullptr，
// `DeskFeedStop` 就会在一条本模块的线程已经在跑的情况下报告"可以卸载"。
bool payload_publish_thread(HANDLE thread) {
    if (thread == nullptr) {
        return false;
    }
    bool expected = false;
    if (!g_thread_published.compare_exchange_strong(expected, true)) {
        return false;
    }
    g_payload_thread = thread;
    g_thread_id = ::GetThreadId(thread);
    return true;
}

DWORD payload_unload_safe() { return g_unload_safe.load(); }

void payload_request_stop() {
    g_stop_requested.store(true);
    DWORD const thread_id = g_thread_id;
    if (thread_id == 0) {
        return;
    }
    // 线程的消息队列在它第一次建窗口或调 PeekMessage 时才存在；刚启动就收到 stop 时要重试几下。
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (::PostThreadMessageW(thread_id, WM_QUIT, 0, 0) != FALSE) {
            return;
        }
        ::Sleep(25);
    }
    log_line(L"PostThreadMessage(WM_QUIT) never succeeded");
}

// 线程入口的**最外层**边界。里面每一步都可能抛（WinRT 的 `hresult_error`、`std::bad_alloc`、
// 字符串构造……），异常穿出线程入口就是未处理异常 = explorer 终止。所以分三道：
//   1. C++ 一层：`catch (...)` 吃掉 C++ 异常；
//   2. SEH 一层：在 C++ 对象全部析构之后才进叶子（MSVC 的 `/EHsc` 下 `catch (...)` 不接管访问
//      违例等非 C++ 异常，所以这是独立的一道，不是重复）；
//   3. 收尾记账（清理事件、卸载门、日志收尾）在**边界之外**做，且自己吞异常——即使前两道都失败，
//      也必须留下"未退休"的结论与观测信号。
void payload_run(HINSTANCE module_instance, std::wstring const& scope) {
    bool crashed = false;
    g_unload_safe.store(0);
    try {
        g_thread_id = ::GetCurrentThreadId();
        // 句柄由 `DeskFeedStart` 在 Resume 之前登记（见 `payload_publish_thread`）。只有那条路径
        // 没走过时（例如自测宿主直接调 `payload_run`）才自己复制一份。
        if (!g_thread_published.load()) {
            HANDLE duplicate = nullptr;
            if (::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(),
                                  &duplicate, SYNCHRONIZE, FALSE, 0) != FALSE) {
                if (payload_publish_thread(duplicate)) {
                    // 登记的句柄归负载所有，模块卸载前不关。
                } else {
                    ::CloseHandle(duplicate);
                }
            }
        }
        log_open(default_log_path(L"deskfeed-payload.log"), false);
        log_format(L"payload thread started: pid=%lu scope=\"%s\"", ::GetCurrentProcessId(),
                   scope.c_str());

        unsigned long fault_code = 0;
        unsigned long long fault_offset = 0;
        run_core_seh(&payload_run_core, module_instance, scope, fault_code, fault_offset);
        if (fault_code != 0) {
            crashed = true;
            safe_log_wide(L"", L"payload thread fault 0x" + hex_text(fault_code) +
                                   L"（相对本模块 +0x" + hex_text(fault_offset) + L"）");
        }
    } catch (winrt::hresult_error const& failure) {
        crashed = true;
        safe_log_wide(L"payload thread stopped on a hresult_error: ", hresult_text(failure.code()));
    } catch (std::exception const& failure) {
        crashed = true;
        safe_log_wide(L"payload thread stopped on an exception: ",
                      deskfeed::wide_from_utf8(failure.what()));
    } catch (...) {
        crashed = true;
        safe_log_line(L"payload thread stopped on an unknown exception");
    }

    safe_finalize(crashed ? 0u : 1u);

    // 返回之后由线程入口回到 kernel32：从那一刻起本模块不再有属于这条线程的指令。
}

// 状态 ABI（见 src/payload/status_pod.h）。由导出的 `DeskFeedGetStatus` 调用。
//
// 纪律：整个函数不携带任何需要析构的局部对象（`__try` 与它们不能共存），异常由调用方那一层
// 的 C++ 边界兜住。`thread_alive` 的判据是**线程句柄没触发**，不是任何标志位。
bool payload_get_status(PayloadStatusPod& pod, std::wstring const& scope) {
    pod.started = g_payload_thread != nullptr ? 1u : 0u;
    HANDLE const thread = g_payload_thread;
    pod.thread_alive =
        (thread != nullptr && ::WaitForSingleObject(thread, 0) == WAIT_TIMEOUT) ? 1u : 0u;
    pod.unload_safe = g_unload_safe.load();
    pod.dangling = hook_post_dangling() ? 1u : 0u;
    pod.mailbox_attached = g_mailbox_attached.load() ? 1u : 0u;
    pod.feed_failures = g_feed_failures.load();
    pod.xaml_block_present = taskbar_xaml_block_visible() ? 1u : 0u;

    // 生产者身份与存活：只在消费者还挂着的时候问。
    DisplayFrameConsumer* consumer = g_consumer.load();
    if (consumer != nullptr) {
        pod.producer_alive = consumer->producer_running() ? 1u : 0u;
        HANDLE const producer = consumer->producer_process();
        if (producer != nullptr) {
            DWORD const pid = ::GetProcessId(producer);
            pod.producer_pid_low = static_cast<std::uint32_t>(pid);
            pod.producer_pid_high = 0;
            FILETIME created{};
            FILETIME exited{};
            FILETIME kernel{};
            FILETIME user{};
            if (::GetProcessTimes(producer, &created, &exited, &kernel, &user) != FALSE) {
                ULARGE_INTEGER stamp{};
                stamp.LowPart = created.dwLowDateTime;
                stamp.HighPart = created.dwHighDateTime;
                pod.producer_start_time = stamp.QuadPart;
            }
        }
    }

    std::size_t const length =
        scope.size() < kPayloadScopeCapacity - 1 ? scope.size() : kPayloadScopeCapacity - 1;
    for (std::size_t index = 0; index < length; ++index) {
        pod.scope[index] = scope[index];
    }
    pod.scope[length] = L'\0';
    return true;
}

}  // namespace deskfeed::payload
