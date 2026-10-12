#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "ipc/control_wire.h"
#include "platform/user_scope.h"

// 启动/控制通道：命名互斥体仲裁单实例，稳定隐藏窗口接受 WM_COPYDATA，
// 发送端用有界的 SendMessageTimeoutW（见 docs/lifecycle.md）。
//
// 载荷规则：接收端在窗口消息处理器里把参数**复制**出来，绝不保存 lpData 指针；
// 载荷里没有发送进程的私有地址，只有协议版本、操作码、发送方 PID 与参数文本。
//
// 线格式（协议常量、`ControlOp` / `ControlReply`、`ControlRequest` 与编解码）在
// `ipc/control_wire.h`：那一份是发送端与接收端的唯一出处，也被任务栏负载 DLL 的
// 单向发送端复用（负载只链编码那一半，不带解码器与校验器）。本文件只管窗口、互斥体与收发。
namespace deskfeed {

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
//
// 这个窗口同时是主进程**唯一那条消息泵**的落脚点：主循环的 MsgWaitForMultipleObjects 与
// 别人（例如右键菜单的 TrackPopupMenu）开的嵌套模态循环，最终都会把窗口消息派发到这里的
// window_proc。所以主进程自己的 WM_APP / WM_TIMER 必须在这里也有去处——只靠主循环里那次
// PeekMessage，会在模态循环期间把取数结果、绑定退出、到期 tick 全部吞掉（见 message_pump）。
class ControlServer {
public:
    using Handler = std::function<ControlReply(ControlRequest const&)>;
    // 主进程自己的窗口消息处理（WM_APP 及以上）与周期心跳。返回 true 表示这条消息已经被
    // 处理掉了，窗口过程不再往下走默认处理（默认处理会把带指针的消息静默丢掉）。
    //
    // 它会在**任何**派发这条消息的循环里被调到：主循环的 PeekMessage，或者模态循环内部的
    // 派发。所以实现必须是可重入安全的（主进程那侧用一把“正在泵”的闸门保证不重入）。
    using Pump = std::function<bool(UINT message, WPARAM wparam, LPARAM lparam)>;

    ~ControlServer();
    ControlServer() = default;
    ControlServer(ControlServer const&) = delete;
    ControlServer& operator=(ControlServer const&) = delete;

    bool create(std::wstring const& class_name, UserSecurity const* security, std::string& error);
    void destroy();
    HWND window() const { return window_; }
    void set_handler(Handler handler) { handler_ = std::move(handler); }
    void set_pump(Pump pump) { pump_ = std::move(pump); }

    // 心跳定时器：毫秒。主循环在模态循环期间不会跑，靠这个定时器把状态机继续推下去
    // （到期 tick、取数结果、绑定退出、发布显示帧）。0 表示不要定时器。
    bool start_heartbeat(UINT interval_ms);
    void stop_heartbeat();

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

private:
    HWND window_ = nullptr;
    std::wstring class_name_;
    Handler handler_;
    Pump pump_;
    bool heartbeat_ = false;
};

// 发送端：找到隐藏窗口并投递。超时或没有实例时返回 false，error 说明原因。
bool control_send(std::wstring const& class_name, ControlRequest const& request,
                  unsigned timeout_ms, ControlReply& reply, std::string& error);

// 只找窗口（没有实例时返回 nullptr）。
//
// 退出类命令需要“等实例真的退出”时，发送端必须在发 `WM_COPYDATA` **之前**抓住窗口属主的真实
// PID 与一个 `SYNCHRONIZE` 句柄：确认返回之后实例可能立刻销毁窗口，那时就再也没有 PID 可抓了。
HWND control_find_window(std::wstring const& class_name);

// 往一个已经找到的窗口发送。线上语义与 control_send 完全一致；window 为 nullptr 时按
// “没有实例”失败返回。
bool control_send_window(HWND window, ControlRequest const& request, unsigned timeout_ms,
                         ControlReply& reply, std::string& error);

// 只查在不在（不投递命令）。
bool control_window_exists(std::wstring const& class_name);

}  // namespace deskfeed
