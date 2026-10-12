#pragma once

#include "payload/status_pod.h"

#include <windows.h>

#include <string>

// 被注入的负载的生命周期。
//
// `DllMain` 只做最小的事：记住模块句柄、`DisableThreadLibraryCalls`、建清理事件。真正的工作在线程上
// 跑——在 `DllMain` 里创建线程会在持有加载器锁时运行它，而负载要激活 WinRT 组件并创建窗口，两者都
// 需要加载器。
namespace deskfeed::payload {

// 由 DllMain 调用：只建清理事件。
void payload_prepare();

// 由 `DeskFeedStart` 在**线程发布之前**调用：把这一次真正被接受的 scope 登记进状态快照，并开一代
// （清上一代的退休证据、unload_safe 归 0、置 started）。状态 ABI 里的 scope 只可能来自这里——
// 没有任何调用方能在查询时指定 scope。
void payload_publish_scope(std::wstring const& scope);

// `ResumeThread` 失败这条 fail-closed 路径用：线程已经登记（句柄归负载所有、绝不能在这里关），但
// 它仍然是 SUSPENDED、永远不会自己退出。把状态快照的 `started` 明确退回 0，这样状态查询不会把
// 一个从未跑过的僵尸线程报成"已启动"；`thread_alive` 仍然由真实句柄决定（未触发 = 1）。
void payload_mark_start_rejected();

// 注入线程的入口。`scope` 为主进程实例命名空间（空 = 生产），负载据此挂上显示邮箱与主进程控制通道。
// 返回时拆除已经完成、清理事件已经置位。
void payload_run(HINSTANCE module_instance, std::wstring const& scope);

// 请求负载线程退出。从任何线程调用都安全（导出的 DeskFeedStop 就是它）。
void payload_request_stop();

// 清理事件；`payload_prepare` 之前是 nullptr。它只是**观测**信号，不是卸载门。
HANDLE payload_cleanup_event();

// 等待负载线程真正终止用的句柄（线程自身的副本）。空表示线程从未起过。
HANDLE payload_thread_handle();

// 由 `DeskFeedStart` 在线程**开始跑之前**登记它创建的线程句柄（`CREATE_SUSPENDED` + 登记 + Resume）。
// 句柄的所有权随之交给负载：它是"线程已终止"这道卸载门要等的对象，由 `payload_release_owned_handles`
// 在 DETACH 时归还；**绝不**在 `DeskFeedStop` 里提前关（`DeskFeedStop` 与随后的状态查询由**同一个
// 串行化 actor** 依次发起，提前关就会在它下一次查询时留下"已关闭、可能被复用"的句柄值）。
// 已经登记过则返回 false（调用方应关掉自己那份）。
bool payload_publish_thread(HANDLE thread);

// 由 `DllMain(DLL_PROCESS_DETACH)` 调用：归还本模块**自己创建**的那两个内核句柄（命名清理事件与
// 负载线程句柄）。只 kernel32 + 栈：不等待、不分配、不加锁、不日志、不碰 user32/COM。
//
// 为什么在 DETACH 关是安全的、也是必需的：唯一合法的卸载路径是 `DeskFeedStop` 返回 1、**并且**调用
// 它的那次远程调用已经退出之后才 `FreeLibrary`（见 docs/native-safety.md 的"退出码与等待"、"进程
// 守卫"两节）。"退出"这条证明按 actor 的串行化纪律成立，而不是"所有 API 都来自同一条 OS 线程"：
// 每一次 API 都可能是另一次 `CreateRemoteThread`；关键是租约 + 那次调用的退出证明保证了此后没有
// `Stop`/`GetStatus` 与卸载并发。所以 DETACH 时刻没有调用方在等这两个句柄，"关掉一个正被并发等待的
// 句柄"结构上不成立。反过来，常年不关就是每次 LoadLibrary 泄漏两个内核句柄——卸载路径越干净，越
// 不该在"装载器已经决定卸载"这一步留下永久引用。
//
// 进程退出时的 DETACH 走的是同一段代码：系统本来就会回收整张句柄表，这里关掉同样无害；本函数不是
// 任何"强卸"路径，也不改变任何卸载结论（结论仍然只由 `DeskFeedStop` 的门禁给出）。
void payload_release_owned_handles() noexcept;

// 卸载门（**模块侧**证据）：1 = 既定回收门禁全部通过，并且本线程已经完成全部模块侧工作（只剩
// `ret`）；0 = 未退休。它**不**包含"线程真的终止"这条证明——那条只能由调用方对自己持有的真实
// 线程句柄等待得到，见 `DeskFeedStop`：两者同时成立才算可以卸载。
DWORD payload_unload_safe();

// 状态 ABI 的填充（见 src/payload/status_pod.h）：把当前状态写进调用方给的 POD。
// 由导出的 `DeskFeedGetStatus` 调用；返回 false 表示这一轮拿不到可信状态。
// **没有 scope 参数**：要报的 scope 是 `payload_publish_scope` 登记的那一个。
bool payload_get_status(PayloadStatusPod& pod);

// `DeskFeedStop` 的重试入口：负载线程已经**真的终止**（或从未起过）之后，把还没拿到证明的回收门
// 再走一遍。内部保证同一时刻只有一个驱动者，且不在任何等锁期间做等待/ping；detail 写明没过的门。
bool payload_retire_after_thread(unsigned timeout_ms, std::wstring& detail);

}  // namespace deskfeed::payload
