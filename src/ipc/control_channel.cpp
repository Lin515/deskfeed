#include "ipc/control_channel.h"

#include "core/utf8.h"
#include "platform/win32_util.h"

#include <windows.h>

#include <cstring>

namespace deskfeed {
namespace {

void put_u32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

bool take_u32(std::string_view bytes, std::size_t& offset, std::uint32_t& value) {
    if (offset + 4 > bytes.size()) {
        return false;
    }
    value = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
    offset += 4;
    return true;
}

}  // namespace

std::intptr_t control_reply_value(ControlReply reply) { return static_cast<std::intptr_t>(reply); }

std::string control_encode(ControlRequest const& request) {
    std::string out;
    put_u32(out, kControlMagic);
    put_u32(out, kControlVersion);
    put_u32(out, static_cast<std::uint32_t>(request.op));
    put_u32(out, request.sender_pid);
    put_u32(out, static_cast<std::uint32_t>(request.args.size()));
    put_u32(out, 0);  // 预留
    for (std::wstring const& arg : request.args) {
        std::string const bytes = utf8_from_wide(arg);
        put_u32(out, static_cast<std::uint32_t>(bytes.size()));
        out.append(bytes);
    }
    return out;
}

bool control_decode(std::string_view bytes, ControlRequest& out, std::string& error) {
    if (bytes.size() > kControlMaxPayloadBytes) {
        error = "control payload too large";
        return false;
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t op = 0;
    std::uint32_t sender = 0;
    std::uint32_t count = 0;
    std::uint32_t reserved = 0;
    if (!take_u32(bytes, offset, magic) || !take_u32(bytes, offset, version) ||
        !take_u32(bytes, offset, op) || !take_u32(bytes, offset, sender) ||
        !take_u32(bytes, offset, count) || !take_u32(bytes, offset, reserved)) {
        error = "control payload truncated";
        return false;
    }
    if (magic != kControlMagic) {
        error = "control magic mismatch";
        return false;
    }
    if (version != kControlVersion) {
        error = "control version mismatch";
        return false;
    }
    if (op != static_cast<std::uint32_t>(ControlOp::Command) &&
        op != static_cast<std::uint32_t>(ControlOp::Inspect)) {
        error = "control op unknown";
        return false;
    }
    if (count > kControlMaxArgs) {
        error = "control argument count out of range";
        return false;
    }

    ControlRequest request;
    request.op = static_cast<ControlOp>(op);
    request.sender_pid = sender;
    request.args.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t length = 0;
        if (!take_u32(bytes, offset, length)) {
            error = "control argument truncated";
            return false;
        }
        if (length > kControlMaxArgBytes || offset + length > bytes.size()) {
            error = "control argument length out of range";
            return false;
        }
        std::string_view const text = bytes.substr(offset, length);
        offset += length;
        if (!utf8_validate(text)) {
            error = "control argument is not valid UTF-8";
            return false;
        }
        request.args.push_back(wide_from_utf8(text));
    }
    if (offset != bytes.size()) {
        error = "control payload has trailing bytes";
        return false;
    }
    out = std::move(request);
    return true;
}

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
    if (message == WM_NCCREATE) {
        auto const* create = reinterpret_cast<CREATESTRUCTW const*>(lparam);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
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
    if (!class_name_.empty()) {
        ::UnregisterClassW(class_name_.c_str(), ::GetModuleHandleW(nullptr));
        class_name_.clear();
    }
    handler_ = nullptr;
}

bool control_window_exists(std::wstring const& class_name) {
    return ::FindWindowW(class_name.c_str(), nullptr) != nullptr;
}

bool control_send(std::wstring const& class_name, ControlRequest const& request,
                  unsigned timeout_ms, ControlReply& reply, std::string& error) {
    HWND const window = ::FindWindowW(class_name.c_str(), nullptr);
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

}  // namespace deskfeed
