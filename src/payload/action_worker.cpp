#include "payload/action_worker.h"

#include "payload/control_upstream.h"
#include "platform/log.h"
#include "platform/win32_util.h"

#include <windows.h>

#include <atomic>
#include <exception>
#include <mutex>
#include <string>

namespace deskfeed::payload {
namespace {

// `g_scope` 与线程句柄都由它保护：工作线程在发请求时读 g_scope，而 start/stop 会写它。
std::mutex g_mutex;
std::wstring g_scope;
HANDLE g_wake = nullptr;  // 自动复位事件：有新请求时唤醒工作线程
HANDLE g_thread = nullptr;
DWORD g_thread_id = 0;
std::atomic<bool> g_quit{false};
std::atomic<bool> g_pending_refresh{false};
std::atomic<bool> g_pending_show{false};
std::atomic<bool> g_running{false};
#ifdef DESKFEED_NATIVE_TESTS
// 见头文件：只让**测试**目标能确定性地制造"线程在期限内没退出"这个真实场景。
std::atomic<unsigned> g_test_sleep_ms{0};
#endif

void send_refresh(std::wstring const& scope) {
    try {
        std::string error;
        if (control_request_refresh(scope, 1500, error)) {
            log_line(L"refresh requested from the taskbar block");
        } else {
            log_format(L"refresh request was not delivered: %S", error.c_str());
        }
    } catch (...) {
        // 这条线程也是本模块的线程：异常绝不许穿出线程入口（那会终止 explorer）。
    }
}

void send_show_widget(std::wstring const& scope) {
    try {
        std::string error;
        if (control_request_show_widget(scope, 1500, error)) {
            log_line(L"desktop widget requested from the taskbar block");
        } else {
            log_format(L"widget request was not delivered: %S", error.c_str());
        }
    } catch (...) {
    }
}

DWORD WINAPI worker_entry(LPVOID) {
    g_running.store(true);
    // 整条线程入口都在 C++ 边界里：作用域字符串的拷贝、发送里的任何分配都可能抛，异常穿出
    // 线程入口就是未处理异常 = explorer 终止。
    try {
        for (;;) {
            ::WaitForSingleObject(g_wake, INFINITE);
            if (g_quit.load()) {
                break;
            }
#ifdef DESKFEED_NATIVE_TESTS
            // 真实的停住：线程确实没有退出，不是把 stop 的结果 mock 掉。`stop` 的超时因此是
            // 真的超时，恢复（sleep 结束后线程看到 quit）也是真的恢复。
            if (unsigned const test_sleep_ms = g_test_sleep_ms.load(); test_sleep_ms != 0) {
                ::Sleep(test_sleep_ms);
            }
#endif
            // 作用域在锁里拷一份：发送期间 start/stop 可能改写 g_scope。
            std::wstring scope;
            {
                std::lock_guard<std::mutex> guard(g_mutex);
                scope = g_scope;
            }
            // 合并后的两类请求各发一次；发送期间的重复请求会落在标志位上，下一轮再发。
            if (g_pending_refresh.exchange(false)) {
                send_refresh(scope);
            }
            if (g_pending_show.exchange(false)) {
                send_show_widget(scope);
            }
        }
    } catch (...) {
        // 记不了日志也要保证"已不在运行"这个结论是真实的。
    }
    g_running.store(false);
    return 0;
}

}  // namespace

bool action_worker_start(std::wstring const& scope, std::wstring& error) {
    std::lock_guard<std::mutex> guard(g_mutex);

    // 已经在跑就**拒绝**，绝不覆盖线程句柄：覆盖会让旧线程变成孤儿（它的句柄再也关不掉，而且
    // 新的 start 会把 quit 复位，旧线程继续活着），而 stop 只停最新那条 ⇒ 调用方会拿到
    // "已终止、可以卸载"的错误答案。要么先 stop，要么复用现有线程。
    if (g_thread != nullptr) {
        error = L"动作工作线程已经在运行";
        return false;
    }
    if (g_wake == nullptr) {
        g_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_wake == nullptr) {
            error = L"建立动作工作线程的唤醒事件失败";
            return false;
        }
    }
    g_scope = scope;
    g_quit.store(false);
    g_pending_refresh.store(false);
    g_pending_show.store(false);
    g_thread = ::CreateThread(nullptr, 0, worker_entry, nullptr, 0, &g_thread_id);
    if (g_thread == nullptr) {
        error = L"建立动作工作线程失败";
        return false;
    }
    try {
        log_format(L"action worker started on thread %lu", g_thread_id);
    } catch (...) {
    }
    return true;
}

void action_worker_post_refresh() {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_thread == nullptr || g_quit.load()) {
        return;
    }
    g_pending_refresh.store(true);  // 合并
    ::SetEvent(g_wake);
}

void action_worker_post_show_widget() {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_thread == nullptr || g_quit.load()) {
        return;
    }
    g_pending_show.store(true);  // 合并
    ::SetEvent(g_wake);
}

bool action_worker_stop(unsigned timeout_ms) {
    HANDLE thread = nullptr;
    HANDLE wake = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        // 先置退出标志再取句柄：即使线程已经退出、句柄已经被这一次 stop 关掉，标志也不会漏。
        g_quit.store(true);
        thread = g_thread;
        wake = g_wake;
    }
    if (thread == nullptr) {
        return true;
    }
    if (wake != nullptr) {
        ::SetEvent(wake);
    }
    if (::WaitForSingleObject(thread, timeout_ms) != WAIT_OBJECT_0) {
        // 线程还在本模块里跑：句柄留着、报告"未退休"，由调用方保留模块。
        log_format(L"the action worker did not stop within %u ms", timeout_ms);
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        ::CloseHandle(thread);
        g_thread = nullptr;
        // 线程已经终止，唤醒事件不再有人等：顺手关掉，别把句柄留在 explorer 的句柄表里。
        if (g_wake != nullptr) {
            ::CloseHandle(g_wake);
            g_wake = nullptr;
        }
        g_scope.clear();
    }
    log_line(L"action worker stopped");
    return true;
}

bool action_worker_running() {
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_running.load();
}

#ifdef DESKFEED_NATIVE_TESTS
void action_worker_probe_set_test_sleep_ms(unsigned ms) { g_test_sleep_ms.store(ms); }
#endif

}  // namespace deskfeed::payload
