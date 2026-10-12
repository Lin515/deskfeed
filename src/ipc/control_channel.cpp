#include "ipc/control_channel.h"

#include "core/utf8.h"  // utf8_from_wide：只用于错误文本；校验器那一份不在这里用
#include "platform/win32_util.h"

#include <windows.h>

#include <cstring>

namespace deskfeed {

// 线格式的编解码在 `ipc/control_wire_encode.cpp` / `control_wire_decode.cpp`：发送端与接收端
// 共用同一份实现，任务栏负载 DLL 只链发送那一半。本文件只负责窗口、互斥体与收发。

// 心跳定时器的 id。窗口只有一个，用一个固定 id 就够了。
constexpr UINT_PTR kHeartbeatTimerId = 1;

InstanceMutex::~InstanceMutex() {
    if (handle_ != nullptr) {
        ::CloseHandle(handle_);
        handle_ = nullptr;
    }
}

bool InstanceMutex::acquire(std::wstring const& name, UserSecurity const* security,
                            bool& already_running, std::string& error) {
    HANDLE const handle = ::CreateMutexW(
        security != nullptr ? const_cast<SECURITY_ATTRIBUTES*>(security->attributes()) : nullptr,
        FALSE, name.c_str());
    if (handle == nullptr) {
        error = "CreateMutex failed: " + utf8_from_wide(last_error_text());
        return false;
    }
    already_running = ::GetLastError() == ERROR_ALREADY_EXISTS;
    if (already_running) {
        // 不是主实例：句柄没有用了，立刻还掉；但名字先留着，方便排障时确认是谁占着。
        ::CloseHandle(handle);
        return true;
    }
    handle_ = handle;
    return true;
}

ControlServer::~ControlServer() { destroy(); }

LRESULT CALLBACK ControlServer::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<ControlServer*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_COPYDATA && self != nullptr && self->handler_) {
        auto const* data = reinterpret_cast<COPYDATASTRUCT const*>(lparam);
        if (data == nullptr || data->dwData != kControlMagic || data->lpData == nullptr ||
            data->cbData > kControlMaxPayloadBytes) {
            return control_reply_value(ControlReply::BadRequest);
        }
        // 必须在这里把参数复制出来：lpData 属于发送进程，消息返回后就无效了。
        std::string const payload(static_cast<char const*>(data->lpData), data->cbData);

        ControlRequest request;
        std::string decode_error;
        if (!control_decode(payload, request, decode_error)) {
            return control_reply_value(ControlReply::BadRequest);
        }
        // 只服务同一 Windows 用户的客户端；别的用户即使拿到窗口句柄也不能提交配置。
        if (!process_user_matches(request.sender_pid)) {
            return control_reply_value(ControlReply::BadRequest);
        }
        return control_reply_value(self->handler_(request));
    }
    if (message == WM_CLOSE) {
        return 0;
    }
    // 主进程自己的窗口消息（WM_APP 及以上）与心跳：交给它自己那条泵。
    //
    // 这一段是必需的，不是可选的：右键菜单的 TrackPopupMenu 会在主进程线程上开一个嵌套模态
    // 循环，主循环那一次 PeekMessage 在这期间根本跑不到；带指针的 WM_APP 若在这里落到
    // DefWindowProc，指针就被静默丢掉（结果丢失 + 内存泄漏）。所以主进程的 WM_APP / WM_TIMER
    // 必须在这个窗口过程里有去处，无论消息是被哪个循环派发过来的。
    if (self != nullptr && self->pump_ != nullptr && message >= WM_APP) {
        if (self->pump_(message, wparam, lparam)) {
            return 0;
        }
    }
    if (message == WM_NCCREATE) {
        auto const* create = reinterpret_cast<CREATESTRUCTW const*>(lparam);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

bool ControlServer::start_heartbeat(UINT interval_ms) {
    if (window_ == nullptr || interval_ms == 0) {
        return false;
    }
    if (::SetTimer(window_, kHeartbeatTimerId, interval_ms, nullptr) == 0) {
        return false;
    }
    heartbeat_ = true;
    return true;
}

void ControlServer::stop_heartbeat() {
    if (window_ != nullptr && heartbeat_) {
        ::KillTimer(window_, kHeartbeatTimerId);
    }
    heartbeat_ = false;
}

bool ControlServer::create(std::wstring const& class_name, UserSecurity const* security,
                           std::string& error) {
    class_name_ = class_name;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &ControlServer::window_proc;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name_.c_str();
    if (::RegisterClassExW(&window_class) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = "RegisterClassEx failed: " + utf8_from_wide(last_error_text());
        return false;
    }
    // 隐藏的顶层窗口（不是 message-only）：message-only 窗口 FindWindow 找不到，
    // 而短命客户端正是靠类名 FindWindow 找到接收端的。它从不 ShowWindow，也不参与任务栏。
    window_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, class_name_.c_str(),
                                L"DeskFeed", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                ::GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        error = "CreateWindowEx failed: " + utf8_from_wide(last_error_text());
        return false;
    }
    (void)security;
    return true;
}

void ControlServer::destroy() {
    if (window_ != nullptr) {
        ::DestroyWindow(window_);
        window_ = nullptr;
    }
    heartbeat_ = false;
    if (!class_name_.empty()) {
        ::UnregisterClassW(class_name_.c_str(), ::GetModuleHandleW(nullptr));
        class_name_.clear();
    }
    handler_ = nullptr;
    pump_ = nullptr;
}

HWND control_find_window(std::wstring const& class_name) {
    return ::FindWindowW(class_name.c_str(), nullptr);
}

bool control_window_exists(std::wstring const& class_name) {
    return control_find_window(class_name) != nullptr;
}

bool control_send_window(HWND window, ControlRequest const& request, unsigned timeout_ms,
                         ControlReply& reply, std::string& error) {
    if (window == nullptr) {
        error = "no instance is listening";
        return false;
    }
    std::string const payload = control_encode(request);
    if (payload.size() > kControlMaxPayloadBytes) {
        error = "control payload too large";
        return false;
    }
    COPYDATASTRUCT data{};
    data.dwData = kControlMagic;
    data.cbData = static_cast<DWORD>(payload.size());
    data.lpData = const_cast<char*>(payload.data());

    DWORD_PTR result = 0;
    // SMTO_ABORTIFHUNG：接收端卡住时立刻失败，不把短命客户端挂死在这里。
    ::SetLastError(ERROR_SUCCESS);
    LRESULT const sent = ::SendMessageTimeoutW(window, WM_COPYDATA, 0,
                                               reinterpret_cast<LPARAM>(&data),
                                               SMTO_ABORTIFHUNG | SMTO_BLOCK, timeout_ms, &result);
    if (sent == 0) {
        DWORD const code = ::GetLastError();
        error = code == ERROR_TIMEOUT ? "the instance did not answer in time"
                                      : "SendMessageTimeout failed: " + utf8_from_wide(last_error_text(code));
        return false;
    }
    reply = static_cast<ControlReply>(static_cast<std::intptr_t>(result));
    return true;
}

bool control_send(std::wstring const& class_name, ControlRequest const& request,
                  unsigned timeout_ms, ControlReply& reply, std::string& error) {
    return control_send_window(control_find_window(class_name), request, timeout_ms, reply, error);
}

}  // namespace deskfeed
