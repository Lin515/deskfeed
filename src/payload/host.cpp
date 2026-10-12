#include "payload/host.h"

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
#include <string>

namespace deskfeed::payload {
namespace {

std::atomic<bool> g_stop_requested{false};
DWORD g_thread_id = 0;
HANDLE g_cleanup_event = nullptr;
HANDLE g_payload_thread = nullptr;
std::atomic<DWORD> g_unload_safe{0};
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

void payload_run(HINSTANCE module_instance, std::wstring const& scope) {
    (void)module_instance;
    g_thread_id = ::GetCurrentThreadId();
    ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(),
                      &g_payload_thread, SYNCHRONIZE, FALSE, 0);
    log_open(default_log_path(L"deskfeed-payload.log"), false);
    log_format(L"payload thread started: pid=%lu scope=\"%s\"",
               ::GetCurrentProcessId(), scope.c_str());

    // 与 explorer 的 DPI 感知对齐，免得子窗口被虚拟化到错误的像素上。
    ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 调度队列必须先于 WinRT 单元：compositor 绑定创建时当前线程的那个队列。
    create_dispatcher_queue();
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (winrt::hresult_error const& failure) {
        log_format(L"init_apartment failed: %s", hresult_text(failure.code()).c_str());
    }

    bool retained = false;
    {
        std::string attach_error;
        DisplayFrameConsumer consumer;
        if (!consumer.attach(scope, attach_error)) {
            // 生产者已经不在了也算失败：据此立刻清理，不留旧读数。
            log_format(L"display mailbox: not attached: %S", attach_error.c_str());
            g_unload_safe.store(1);
        } else {
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
                input.owner_alive = true;
                input.have_last_frame = have_last;
                input.failures = failures;
                FeedOutcome const decision = feed_decide(input);
                if (decision == FeedOutcome::Apply) {
                    have_last = true;
                    failures = 0;
                    taskbar_xaml_request_update(reading_from_frame(frame));
                    if (frame.closed) {
                        log_line(L"the display mailbox closed by the producer");
                        break;
                    }
                } else if (decision == FeedOutcome::Keep) {
                    ++failures;
                } else {
                    log_line(L"the reading is dropped (no complete frame and no usable last frame)");
                    taskbar_xaml_request_update(ReadingContent{});
                    failures = 0;
                    have_last = false;
                    break;
                }
            }

            // 拆除顺序：动作工作线程 → 任务栏块与悬停/菜单 → 钩子的静止证明。任何一步没有拿到
            // "已终止/已证明"都算未退休，绝不能报告可卸载。
            if (!action_worker_stop(4000)) {
                retained = true;
            }
            std::wstring detach_detail;
            if (!taskbar_xaml_detach(4000, detach_detail)) {
                retained = true;
                log_format(L"teardown did not retire everything: %s", detach_detail.c_str());
            }
            xaml_tap_stop();
            consumer.detach();
        }
    }

    if (hook_post_dangling()) {
        retained = true;
        log_line(L"a hook is still dangling; the module must stay loaded");
    }

    // 调度队列刻意**不**经 ShutdownQueueAsync 关：它的完成回调会回到本模块，上一次实测直接把
    // explorer 打崩。丢掉控制器就够了，队列随线程消失。
    g_dispatcher_controller = nullptr;

    g_unload_safe.store(retained ? 0 : 1);
    log_format(L"payload stopped (unload safe: %s)", retained ? L"no" : L"yes");
    log_close();
    if (g_cleanup_event != nullptr) {
        ::SetEvent(g_cleanup_event);
    }
    // 返回之后由线程入口回到 kernel32：从那一刻起本模块不再有属于这条线程的指令。
}

}  // namespace deskfeed::payload
