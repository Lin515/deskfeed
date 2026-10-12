#pragma once

#include "payload/payload_flavor.h"

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
#include "payload/probe_api.h"
#endif

#include <windows.h>

#include <string>

// 把一次动作编组到任务栏线程上，并给这条路径一个**可证明的收尾**。
//
// 为什么是线程级 WH_GETMESSAGE 钩子：XAML 元素有线程亲和性，只有任务栏自己的线程能碰托盘
// 那棵树；`SetWindowSubclass` 拒绝跨线程调用，而替换 `Shell_TrayWnd` 的窗口过程实测会让
// explorer 在大约十秒后崩溃。线程钩子是在目标线程下一次取消息时跑我们的代码，干完就自摘，
// 任务栏线程上不留任何常驻改动。
//
// 收尾协议（公开说明见 docs/native-safety.md 的"任务栏线程编组"一节）：
//   * **只在钩子过程内部卸钩**：`UnhookWindowsHookEx` 的文档说明，从别的线程卸钩返回之后本次
//     调用仍可能在跑、并发时甚至不保证已移除；而"在钩子过程里自摘"属于 `not being called
//     concurrently` 那一支，移除发生在本回调返回之前。所以钩子句柄集中成一个 `std::atomic<HHOOK>`，
//     **只有回调**用 `exchange(nullptr)` 取走并卸钩；超时路径绝不强摘。
//   * **安装/发布握手**：`SetWindowsHookExW` 返回之后、调用方把句柄存进原子变量之前，任务栏线程
//     就可能已经进入回调。所以安装方在存好句柄之后置一个"已发布"事件，回调取不到句柄时**有界地**
//     等它一次再取；仍取不到就什么都不做（记录留在链上，下一次取消息再跑），绝不出现"动作跑了、
//     钩子却没卸"的记录。
//   * **`hMod` 传 NULL**：`SetWindowsHookExW` 要求，当线程属于当前进程且钩子过程也在当前进程代码里
//     时 `hMod` 必须是 NULL。传模块句柄是未定义行为，也不能指望 user32 因此多持引用。
//   * **自引用在装钩之前取**：`GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)`（**不是**
//     `PIN`）给本模块加一次普通加载器引用，保证"user32 已经决定调用、但还没进入"的那一瞬间模块
//     仍然映射着。正常路径上由 `hook_post_retire` 在拿到"帧已弹掉"的证明之后归还一次，模块引用数
//     回到调用方那一份（验收判据 `payload module references at the end: 1` 不变）。**安装失败也会
//     立刻归还**，不许留下多余的引用。
//   * **完成事件是唯一的收尾门**：一个手动复位事件在回调的**最后一步**置位（此后只剩裸返回）。
//     收尾必须等它，**不能**拿"钩子句柄已经空了"或"在途计数为零"冒充完成——句柄是在回调开头被取走的，
//     那时执行体、`CallNextHookEx` 与它们的析构都还没跑完，第三方钩子可能就在那里泵消息。
//   * **"帧已弹掉"的证明是一次非排队消息**：完成事件之后，收尾线程向 T 拥有的窗口发一次
//     `SendMessageTimeout(WM_NULL)`。跨线程 SendMessage 只有目标线程回到消息检索路径、执行完一次
//     窗口过程才可能返回，而钩子过程是 `GetMessage`/`PeekMessage` 调用栈里的内层帧——所以这次成功
//     返回就证明钩子帧已经弹掉，此后才归还自引用。发 ping 前后都核对该窗口的属主线程与属主进程，
//     确保"同一条 T、同一个进程"没有变。
//   * **回调里不做任何可能分配或失败的事**：执行体与其结果发布都在 C++ 与 SEH 边界内完成，
//     `CallNextHookEx` 之后只剩一次 `SetEvent` 与裸返回；日志与字符串整理全部留给调用方。
//   * **超时不是失败，是"这次不退休"**：不卸钩、不归还引用、不报告可卸载。记录留在 T 的链上，
//     T 下一次取消息时会自己跑完并自摘；稍后的第二次调用可以幂等重试。
//   * 这条路径**不替换任何窗口过程**，也**不调用** `ShutdownQueueAsync`。
namespace deskfeed::payload {

enum class HookAction {
    Apply,
    Remove,
};

// 真正在任务栏线程上跑的那件事。返回 false 表示动作失败，原因写进 error。
// 执行体**不得**在这里泵消息（GetMessage/PeekMessage/MsgWaitForMultipleObjects/SendMessage/
// DialogBox 等）：那会让待处理的 ping 在自己的钩子帧里被处理，收尾证明就不成立了。
using HookExecutor = bool (*)(void* context, HookAction action, std::wstring& error);

// 由负载在启动时注册一次。context 一般指向任务栏那块自己的状态。
void hook_post_set_executor(HookExecutor executor, void* context);

struct HookPostResult {
    bool ok = false;         // 执行体报告成功
    bool retired = false;    // 钩子记录已被 T 在回调内摘掉（记录已移除）
    bool armed = false;      // 本轮真的装上过钩子
    std::wstring error;      // 失败原因（进日志）
};

// 投递一次动作到任务栏线程并等完成事件。**不**在这里归还自引用，也**不**在这里强行收尾。
// `tray_thread_id` 必须是持有托盘窗口的那个线程（`GetWindowThreadProcessId(Shell_TrayWnd)`）。
HookPostResult hook_post_run(DWORD tray_thread_id, HookAction action, unsigned timeout_ms);

// 收尾：等**完成事件** → 非排队 ping 证明钩子帧已弹掉 → 才归还自引用。
// 返回 false 表示**未退休**（完成事件没到、记录没被摘掉、或 ping 没拿到证明）：调用方必须保留模块、
// 不得报告"可卸载"。detail 写明是哪一条证明没拿到，进日志。
bool hook_post_retire(HWND ack_window, DWORD tray_thread_id, unsigned timeout_ms,
                      std::wstring& detail);

// 是否还留着一次没有证明安全的悬挂（收尾判定用）。
bool hook_post_dangling();

// 是否还有装着没退休的钩子记录。
bool hook_post_armed();

// 自测宿主专用：没有 Shell_TrayWnd 时，用它指定的、**属于同一条 UI 线程**的窗口当收尾 ping 的
// 确认目标。必须是 user32 类的窗口（例如一个 STATIC 子窗口），绝不能用本模块自己的窗口过程——
// 那会引入新的 `ret` 问题。
void hook_post_set_ack_window(HWND window);

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
// 自测宿主专用：把"存好钩子句柄"这一步人为推迟若干毫秒，用来**确定性地**制造
// "回调在安装发布之前就进入"这个对抗场景。0 = 关闭（默认）。生产 DLL 里没有这条路径。
void hook_post_set_install_delay_ms(unsigned delay_ms);

// ---------------------------------------------------------------------------
// 探针 ABI 的内部实现（导出的入口见 src/payload/dllmain.cpp 的 DeskFeedProbe* 一组，
// 共享 POD 定义见 src/payload/probe_api.h）。这些入口只在自测宿主进程里被调用。
// ---------------------------------------------------------------------------

// 把钩子门的原子状态填进 POD。`out.size` 必须等于编译时的结构大小。
bool hook_post_probe_status(HookProbeStatus& out);

// 让负载记录"回调走到了哪个阶段"。返回 false 表示当前没有正在被观测的安装。
bool hook_post_probe_observer_stage(HookProbeStage& out);

// 打开/关闭阶段记录。关闭会把记录清干净（stage = None、sequence = 0）。
void hook_post_probe_set_observer(bool enable);

// 完成事件变异模式（**只**给探针的对抗用例；`DeskFeedStart` 会把它复位成 Normal）。
//
// 粘性纪律（只有自测构建有这一段）：一旦有人用过变异，本轮**不许**产出"可以卸载"的结论 ——
// 提前置位的完成事件会让"帧里的动作已经跑完"看起来成立，而那一刻我们那一帧还压在 T 的栈上。
// `hook_post_retire` 见到粘性标记就返回"未退休"（不归还自引用）；调用方要拿正常证据，必须先把
// 变异关回去、并确认回调真的已经返回。
void hook_post_probe_set_completion_mutation(CompletionMutation mode);

// 收尾时"帧已弹掉"的 ping 目标。
void hook_post_probe_set_tray_window(HWND window);

// 探针"已布防"：为真时 `DeskFeedStart` **不**复位探针开关（R5 指定的托盘窗口必须活过 Start，
// 真实 mailbox 的 UI 投递才能落在自测宿主的线程上）。只有探针显式置位才生效。
bool hook_post_probe_armed();
void hook_post_probe_set_armed(bool armed);
#endif  // DESKFEED_PAYLOAD_PROBE_BUILD

}  // namespace deskfeed::payload
