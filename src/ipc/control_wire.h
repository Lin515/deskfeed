#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 启动/控制通道的**线格式**：协议版本、操作码、请求载荷与编解码。
//
// 这一份刻意与窗口/互斥体/收发拆开（见 `control_channel.h` 与 `control_wire_*.cpp`），
// 为的是让任务栏负载 DLL 只链**发送那一半**（`deskfeed_control_wire_send`）：负载只会
// “发一个动作给主进程”，它不需要接收端的解码器（那里面才有 `utf8_validate` 这类校验器），
// 也不需要 `ControlServer` / `InstanceMutex`。这样负载的源码图里不会出现 JSON、网络或协议校验器。
//
// 命名规则（发送端与接收端共用的唯一出处）：
//   * 控制窗口类名 = `scoped_name(kControlWindowClassBase, scope)`；scope 为空即生产实例，
//     用户 SID 的短哈希由 `scoped_name` 统一加（与显示邮箱同一套命名，不另造后缀）。
namespace deskfeed {

// 控制窗口类名的基础部分。实际类名由 platform/paths.h 的 scoped_name 按 scope 与当前用户展开。
inline constexpr wchar_t kControlWindowClassBase[] = L"DeskFeedControl";
inline constexpr std::uint32_t kControlMagic = 0x31464644;  // 'D','F','F','1'
inline constexpr std::uint32_t kControlVersion = 1;
inline constexpr std::size_t kControlMaxPayloadBytes = 64 * 1024;
inline constexpr std::size_t kControlMaxArgs = 64;
inline constexpr std::size_t kControlMaxArgBytes = 4096;

enum class ControlOp : std::uint32_t {
    Command = 1,  // args 是原始命令行参数，由接收端照常解析
    Inspect = 2,  // 查询运行状态
    // 任务栏组件向主进程提出的两个**动作请求**。数值一经定下不再改动，解码端按白名单校验。
    //
    // 它们与 Command 共用同一条 WM_COPYDATA 通道、同一份 scope / 用户 / 会话命名与同一个控制
    // 窗口类：载荷 DLL 不需要新的协议、不自己启动任何进程，也不改变 URL、PID 绑定或调度配置。
    // 发送方必须是**当前任务栏属主**那个 explorer 进程（接收端按发送方 PID 核对属主与属主 SID），
    // 所以这两个动作不是“任何进程都能远程触发主进程行为”的入口。
    //
    // 应答语义：Accepted 只表示**动作已排进主进程的消息队列**，不表示取数已经完成、更不表示
    // 收到了 200。窗口过程在这里绝不做网络或磁盘工作，也不等待。
    Refresh = 3,     // args 为空：立刻对**当前**地址取一次数（不追加任何参数、不改调度）
    ShowWidget = 4,  // args 为空：把桌面悬浮窗叫出来（主进程负责取消隐藏意图并落盘）
    // 托盘屏幕配置：args 是完整的启用屏幕标识表（0..kMaxTaskbarScreens 项）。
    // 0 项表示用户明确选择“不在任何任务栏显示”；配置落盘后主进程会重发显示帧，
    // 负载据此增删各屏幕上的块。**退化（某屏幕不可用而落到主屏）不会发这个动作**，
    // 所以它永远不会改写配置。
    SetScreens = 5,
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
// 编码只需要 UTF-8 转换（`deskfeed_core_text`）；解码还需要 `utf8_validate`（`deskfeed_core`）。
std::string control_encode(ControlRequest const& request);
bool control_decode(std::string_view bytes, ControlRequest& out, std::string& error);

}  // namespace deskfeed
