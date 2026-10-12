#pragma once

#include <string>

// 负载 → 主进程 的**单向**控制客户端：任务栏那块上的两个交互（双击 / 右键菜单）把请求交给主进程，
// 由主进程决定“立刻重新取数”或“把桌面悬浮窗叫出来”。
//
// 走的是 Phase 1 就有的控制通道（`ipc/control_wire.h` 的线格式 + 隐藏窗口类），**不是**任何新的
// 进程、不接受任何参数、不改变 URL、不做绑定、不改调度。负载只链编码那一半，所以它的源码图里
// 不会出现 JSON、网络或协议校验器。
//
// 命名与 scope 完全复用现有规则：类名 = `scoped_name(kControlWindowClassBase, scope)`，
// 与显示邮箱同一套 scope / 用户 / 会话命名（scope 为空即生产实例）。
namespace deskfeed::payload {

// 请求主进程立刻对当前地址取一次数。返回 false 时 error 说明原因（没有实例、超时、被拒绝）。
bool control_request_refresh(std::wstring const& scope, unsigned timeout_ms, std::string& error);

// 请求主进程把桌面悬浮窗叫出来。
bool control_request_show_widget(std::wstring const& scope, unsigned timeout_ms, std::string& error);

}  // namespace deskfeed::payload
