#pragma once

#include <string>

// 让 XAML 的"composition 走查器"判定读到 `DisableCompositionDiag=1`。
//
// 为什么需要它：启用 XAML 诊断（InitializeXamlDiagnosticsEx / AdviseVisualTreeChange）时，系统
// XAML 会额外创建一套 composition 走查器（CreateCompVisualDiag）。它会在每次有 DirectComposition
// 视觉加入时**无锁地重建**进程级的走查器，两个 UI 线程同时进入就会踩坏堆。规避办法是让
// `HKLM\Software\Microsoft\XAML\Debug` 下 `DisableCompositionDiag` 这一次读取返回 1：XAML 读完
// 会缓存结论。
//
// 本机的实际情况：那个键**根本不存在**，所以 `RegOpenKeyExW` 会失败，XAML 连值都不会去读。于是
// 这里做两件事：给 `RegOpenKeyExW` 的替身在我们自己写的替身键（`HKCU\Software\DeskFeed\XamlDebug`，
// 里面有 `DisableCompositionDiag=1` 与一个标记值）上"变"出一个成功的打开结果；`RegQueryValueExW`
// / `RegGetValueW` 的替身认出这个键，并只对 `DisableCompositionDiag` 回答 DWORD 1。
//
// 钩子方式：**按名字改各模块的导入地址表（IAT）**，不写内联跳转。IAT 槽是数据而不是代码：不需要
// 反汇编、不需要可执行内存、卸载就是把原值写回去。这一步要求负载以 `/guard:cf` 编译：开启 CFG 的
// 进程会校验每一个间接调用目标，而 IAT 槽指的就是本模块里的函数。
//
// ## 明确接受、无法证明的残余窗口（不是"已证明安全"）
//
// 任一线程都可能正好停在 `call [IAT]` 与替身第一条指令之间。`in_flight` 计数与恢复 IAT 都**消不掉**
// 这个单指令窗口：user32/ntdll 层没有任何东西能观察"已经决定调用、但还没进入"的状态。
// 本项目在 MAIN 裁决下**明确接受**这一点，理由是保持 XAML 平台兼容、且注入只发生在用户授权的
// 受控测试里；同时已尽力减险：
//   * 走查（唯一需要这层规避的阶段）结束后**立刻恢复 IAT**，而不是在整个常驻生命里一直拦；
//   * 恢复时先把全部槽原子写回，再等**观测到的**在途调用归零；
//   * 原函数地址与替身键一直保留到最后一次观测到的回调结束，不提前清；
//   * 恢复失败（还有槽指着本模块）时 `reg_hook_restore` 返回 false，负载据此让 `DeskFeedStop`
//     返回"未退休"，注入器**不得**卸载。
//
// 因此 `DeskFeedStop` 返回 1 表示**既定回收门禁通过**，不代表"绝对安全"；详细口径见
// docs/taskbar-injection.md。
namespace deskfeed::payload {

// 装钩子：准备好替身键，并给当前已加载的每个模块补上导入表项。必须在真正连接诊断之前调用。
// 失败不算致命（退化成"不做这层规避"的旧行为），原因写进 error 与日志。
bool reg_hook_install(std::wstring& error);

// 把后来装载进来的模块也补上（连接诊断的过程会载入新模块）。返回本次新改的槽数。
unsigned reg_hook_rescan();

// 恢复：先把全部槽写回原值（不再有新入口走替身），再等在途调用归零。
// 返回 false 表示**还有槽指着本模块**或没能确认零在途——负载必须保留映射。
bool reg_hook_restore(std::wstring& error);

bool reg_hook_installed();

// 本进程与本模块的 CFG 状态，供日志记录这层规避的前提是否成立。
std::wstring reg_hook_cfg_state();

// 自检：完全按 XAML 的做法读一次那个值，把每一步的返回值与数据写进 detail。
// 装钩子之前应当看到"键不存在"，装好之后应当看到 1——这一前一后就是钩子在起作用的证据。
bool reg_hook_probe_read(std::wstring& detail);

}  // namespace deskfeed::payload
