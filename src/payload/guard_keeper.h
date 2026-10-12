#pragma once

// 负载侧（Native）持有的**进程守卫对象**：租约 mutex、粘性不确定 event、入口记录 mapping。
//
// 契约、40 字节记录布局、状态值与恢复规则全部在 `payload/guard_record.h`（唯一出处），本文件只声明
// 三个动作。它不是"工具租约"的复制品：工具侧（`src/inject/guard.*`）是 actor，持锁读记录并负责
// 清/置事件；这里是 Native，**只读不可变头 + 观查 event**，永不写 [8,40) 快照、永不置/清 event。
//
// 三条硬约束（改动前先读 `guard_record.h` 顶部注释与 `docs/native-safety.md`）：
//   1. `payload_guard_prepare()` 只会从 `DllMain(DLL_PROCESS_ATTACH)` 调用 ⇒ 跑在**加载器锁**里。
//      因此它只允许 kernel32 调用 + 栈上定长缓冲：不等待、不分配、不 user32/COM/Registry/SID、
//      不文件 I/O、不日志。
//   2. `payload_guard_stop_allowed()` 是 `DeskFeedStop` 的**第一道门**。任何"不可证"（对象没建起来、
//      事件已置位、不可变头不认、坏视图）都返回 false = 未退休 = 必须保留映射。它绝不因为
//      "actor 自己正在执行的那次 Stop 已经写下 Pending* "而拒绝——那正是必须允许走到结论的调用。
//   3. `payload_guard_release()` 只会从 `DllMain(DLL_PROCESS_DETACH)` 调用 ⇒ 同样只允许 kernel32：
//      只关本模块自己那 3 个句柄与那个只读视图，**不等待**。它不承载任何事实：唯一合法的卸载路径是
//      `DeskFeedStop` 返回 1 之后的 `FreeLibrary`，而未知/事件置位时 Native 绝不返回 1。
//
// 范围限定：只对本协议版本的 cooperating 参与者成立（本仓库 `deskfeed-inject.exe` 与负载 DLL）。
// 不成立于：旧版/第三方工具、调试器、手工 `FreeLibrary`、不拿租约的调用者；外部线程入口
// （`DllGetClassObject`、XAML tap 回调、探针入口）也不在这套门里。不宣称"整进程任意线程都安全"。

namespace deskfeed::payload {

// 由 `DllMain(DLL_PROCESS_ATTACH)` 调用，只做一次。失败（或记录不可信）只置粘性"未知"，
// 不让 DLL 加载失败。
void payload_guard_prepare() noexcept;

// `DeskFeedStop` 的第一道门：false = 未知/粘性不确定/不可变头不认 ⇒ 未退休（保留映射）。
bool payload_guard_stop_allowed() noexcept;

// 由 `DllMain(DLL_PROCESS_DETACH)` 调用：只关自己那 3 个句柄与只读视图，不等待、不分配。
void payload_guard_release() noexcept;

}  // namespace deskfeed::payload
