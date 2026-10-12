#include "payload/action_worker.h"

#include "payload/control_upstream.h"
#include "platform/log.h"

#include <windows.h>

#include <atomic>

namespace deskfeed::payload {
namespace {

std::wstring g_scope;
HANDLE g_wake = nullptr;  // 自动复位事件：有新请求时唤醒工作线程
HANDLE g_thread = nullptr;
DWORD g_thread_id = 0;
std::atomic<bool> g_quit{false};
std::atomic<bool> g_pending_refresh{false};
std::atomic<bool> g_pending_show{false};
std::atomic<bool> g_running{false};

void send_refresh() {
    std::string error;
    if (control_request_refresh(g_scope, 1500, error)) {
        log_line(L"refresh requested from the taskbar block");
    } else {
        log_format(L"refresh request was not delivered: %S", error.c_str());
    }
}

void send_show_widget() {
    std::string error;
    if (control_request_show_widget(g_scope, 1500, error)) {
        log_line(L"desktop widget requested from the taskbar block");
    } else {
        log_format(L"widget request was not delivered: %S", error.c_str());
    }
}

DWORD WINAPI worker_entry(LPVOID) {
    g_running.store(true);
    for (;;) {
        ::WaitForSingleObject(g_wake, INFINITE);
        if (g_quit.load()) {
            break;
        }
        // 合并后的两类请求各发一次；发送期间的重复请求会落在标志位上，下一轮再发。
        if (g_pending_refresh.exchange(false)) {
            send_refresh();
        }
        if (g_pending_show.exchange(false)) {
            send_show_widget();
        }
    }
    g_running.store(false);
    return 0;
}

}  // namespace

bool action_worker_start(std::wstring const& scope, std::wstring& error) {
    g_scope = scope;
    if (g_wake == nullptr) {
        g_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_wake == nullptr) {
            error = L"建立动作工作线程的唤醒事件失败";
            return false;
        }
    }
    g_quit.store(false);
    g_thread = ::CreateThread(nullptr, 0, worker_entry, nullptr, 0, &g_thread_id);
    if (g_thread == nullptr) {
        error = L"建立动作工作线程失败";
        return false;
    }
    log_format(L"action worker started on thread %lu", g_thread_id);
    return true;
}

void action_worker_post_refresh() {
    if (g_thread == nullptr || g_quit.load()) {
        return;
    }
    g_pending_refresh.store(true);  // 合并
    ::SetEvent(g_wake);
}

void action_worker_post_show_widget() {
    if (g_thread == nullptr || g_quit.load()) {
        return;
    }
    g_pending_show.store(true);  // 合并
    ::SetEvent(g_wake);
}

bool action_worker_stop(unsigned timeout_ms) {
    if (g_thread == nullptr) {
        return true;
    }
    g_quit.store(true);
    ::SetEvent(g_wake);
    if (::WaitForSingleObject(g_thread, timeout_ms) != WAIT_OBJECT_0) {
        // 线程还在本模块里跑：句柄留着、报告"未退休"，由调用方保留模块。
        log_format(L"the action worker did not stop within %u ms", timeout_ms);
        return false;
    }
    ::CloseHandle(g_thread);
    g_thread = nullptr;
    log_line(L"action worker stopped");
    return true;
}

bool action_worker_running() { return g_running.load(); }

}  // namespace deskfeed::payload
