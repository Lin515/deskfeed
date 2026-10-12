#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/user_scope.h"

// 启动/控制通道：命名互斥体仲裁单实例，稳定隐藏窗口接受 WM_COPYDATA，
// 发送端用有界的 SendMessageTimeoutW（见 docs/lifecycle.md）。
//
// 载荷规则：接收端在窗口消息处理器里把参数**复制**出来，绝不保存 lpData 指针；
// 载荷里没有发送进程的私有地址，只有协议版本、操作码、发送方 PID 与参数文本。
namespace deskfeed {

inline constexpr wchar_t kControlWindowClassBase[] = L"DeskFeedControl";
inline constexpr std::uint32_t kControlMagic = 0x31464644;  // 'D','F','F','1'
inline constexpr std::uint32_t kControlVersion = 1;
inline constexpr std::size_t kControlMaxPayloadBytes = 64 * 1024;
inline constexpr std::size_t kControlMaxArgs = 64;
inline constexpr std::size_t kControlMaxArgBytes = 4096;

enum class ControlOp : std::uint32_t {
    Command = 1,  // args 是原始命令行参数，由接收端照常解析
    Inspect = 2,  // 查询运行状态
};

enum class ControlReply : std::intptr_t {
    Accepted = 0,   // 命令已被接受
    Rejected = 1,   // 命令被拒绝（地址非法、保存失败等）
    BadRequest = 2, // 载荷/参数不合法
    NotBound = 3,   // --quit-if-bound 的 PID 不匹配（对发送端算成功）
};

std::intptr_t control_reply_value(ControlReply reply);

struct ControlRequest {
    ControlOp op = ControlOp::Command;
    std::uint32_t sender_pid = 0;
    std::vector<std::wstring> args;
};

// 序列化/反序列化（同一份代码给发送端与接收端用，两端不可能对线格式有第二种解释）。
std::string control_encode(ControlRequest const& request);
bool control_decode(std::string_view bytes, ControlRequest& out, std::string& error);

// 单实例仲裁：CreateMutexW 的返回值 + ERROR_ALREADY_EXISTS 决定谁是主实例。
class InstanceMutex {
public:
    ~InstanceMutex();
    InstanceMutex() = default;
    InstanceMutex(InstanceMutex const&) = delete;
    InstanceMutex& operator=(InstanceMutex const&) = delete;

    // already_running 为 true 时表示本会话里已经有实例（此时 handle 仍然有效，
    // 调用方应当转发命令然后退出，不要做任何常驻工作）。
    bool acquire(std::wstring const& name, UserSecurity const* security, bool& already_running,
                 std::string& error);

private:
    HANDLE handle_ = nullptr;
};

// 接收端：隐藏窗口 + WM_COPYDATA。必须建在主实例的消息线程上。
class ControlServer {
public:
    using Handler = std::function<ControlReply(ControlRequest const&)>;

    ~ControlServer();
    ControlServer() = default;
    ControlServer(ControlServer const&) = delete;
    ControlServer& operator=(ControlServer const&) = delete;

    bool create(std::wstring const& class_name, UserSecurity const* security, std::string& error);
    void destroy();
    HWND window() const { return window_; }
    void set_handler(Handler handler) { handler_ = std::move(handler); }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

private:
    HWND window_ = nullptr;
    std::wstring class_name_;
    Handler handler_;
};

// 发送端：找到隐藏窗口并投递。超时或没有实例时返回 false，error 说明原因。
bool control_send(std::wstring const& class_name, ControlRequest const& request,
                  unsigned timeout_ms, ControlReply& reply, std::string& error);

// 只查在不在（不投递命令）。
bool control_window_exists(std::wstring const& class_name);

}  // namespace deskfeed
