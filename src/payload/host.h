#pragma once

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

// 注入线程的入口。`scope` 为主进程实例命名空间（空 = 生产），负载据此挂上显示邮箱与主进程控制通道。
// 返回时拆除已经完成、清理事件已经置位。
void payload_run(HINSTANCE module_instance, std::wstring const& scope);

// 请求负载线程退出。从任何线程调用都安全（导出的 DeskFeedStop 就是它）。
void payload_request_stop();

// 清理事件；`payload_prepare` 之前是 nullptr。它只是**观测**信号，不是卸载门。
HANDLE payload_cleanup_event();

// 等待负载线程真正终止用的句柄（线程自身的副本）。空表示线程从未起过。
HANDLE payload_thread_handle();

// 卸载门：1 = 既定回收门禁全部通过；0 = 未退休（必须保留映射）。线程没结束前一律是 0。
DWORD payload_unload_safe();

}  // namespace deskfeed::payload
