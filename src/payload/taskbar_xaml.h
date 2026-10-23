#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "display/reading.h"
#include "payload/payload_flavor.h"

// 把主进程算出来的**通用读数**插进任务栏的 XAML 树，并让任务栏按自己的布局给它腾地方。
//
// 四条硬事实决定了这个模块的形状：
//  * XAML 元素有线程亲和性。碰元素的一切都必须在拥有任务栏 XAML 的那条线程上，也就是
//    Shell_TrayWnd / Shell_SecondaryTrayWnd 的线程；所以工作经线程级 WH_GETMESSAGE 钩子编组过去
//    （`payload/hook_post.*`），调用方只投递并等待。
//  * 树来自公开的 XAML 诊断接口（`payload/xaml_tap.h`），不是 shell 的内部对象。每个 XAML 核心
//    在**一次注入的生命周期里最多走查一次**；多屏时逐核心走查到所有托盘元素都拿到为止，随后立刻
//    关掉诊断连接，运行期只换字、增删块。
//  * 显示内容与“在哪些屏幕显示”都来自主进程发布的最新显示帧。
//  * 读数只在进程内缓存（本文件的 mutex 保护的那份拷贝）：负载线程写、任务栏线程读。
namespace deskfeed::payload {

// 自测宿主的走查入口：在调用方自己的线程上跑（诊断会把工作编组到核心线程并等它，在任务栏线程上
// 调用会死锁）。它挑与目标同线程的一个 XAML 核心走查一次，记下所有同名元素的句柄；**不重试**。
bool taskbar_xaml_watch(std::wstring& error);

// 负载线程调用：装好 composition 诊断规避的钩子、连接本进程的 XAML 核心、按核心逐个走查，
// 把元素解析并映射到屏幕（解析在任务栏线程上完成），再按配置插入第一份读数；最后**立刻**关掉
// 诊断连接并恢复导入表——规避钩子只需要覆盖走查这一段，不覆盖整个常驻生命。
bool taskbar_xaml_attach(std::wstring const& scope, std::wstring& error);

// 显示内容变了：从负载线程调用。只投递一次"就地换字"，绝不再走查、不再连诊断。
bool taskbar_xaml_request_update(deskfeed::ReadingContent const& content);

// 主进程发布的托盘屏幕配置（显示帧 v2）。`configured=false` 表示按“仅主屏”的缺省处理；
// configured 为真且 screens 为空表示明确不在任何任务栏显示。它只更新配置并触发一次目标
// 协调（增删各屏的块），绝不重新连接 XAML 诊断、也绝不重新走查。
void taskbar_xaml_set_screen_config(bool configured, std::vector<std::wstring> const& screens);

// 拆除：摘块 → 收悬停与菜单 → 退休钩子（等静止证明）。返回 false 表示**有一条证明没拿到**，
// 调用方必须保留模块、不得报告可卸载；detail 写明原因。
bool taskbar_xaml_detach(unsigned timeout_ms, std::wstring& detail);

// 块现在在不在树里（自测宿主与日志用）。
bool taskbar_xaml_block_visible();

// 自测宿主入口：调用线程就是树所属的线程，所以"解析 + 应用读数"直接在本线程做。
bool taskbar_xaml_probe_run(bool dump_only, bool allow_placeholder, bool& visible,
                            std::wstring& error);

// 自测宿主专用：把块与每个来源的屏幕矩形交给探针（探针再按它合成鼠标输入验悬停）。
int taskbar_xaml_probe_hover_cells(long* out, int capacity);

// 自测宿主专用：让任务栏线程上的"应用读数"刻意慢下来，用来验"挂着的时候 stop 必须未退休"。
void taskbar_xaml_probe_set_slow_apply(unsigned ms);

// 本次注入的读数：**生产路径**（负载线程读到第一帧、以及每次换内容）也走这个入口，它同时是
// 探针宿主放占位读数的入口。它只把读数写进任务栏块自己的内容槽，不碰 XAML 结构，也不依赖邮箱。
void taskbar_xaml_set_reading(deskfeed::ReadingContent const& content);

// 自测宿主专用：本线程就是树所属的线程，拆除时不必再投递（也无 Shell_TrayWnd 可投）。
void taskbar_xaml_set_probe_inline(bool inline_mode);

// 自测宿主专用：宿主自己的宿主窗口。它同时作为悬停提示的属主与钩子收尾那次
// `SendMessageTimeout(WM_NULL)` 的确认窗口——必须是**属于同一条 UI 线程**的、user32 类的窗口。
void taskbar_xaml_set_host_window(HWND window);

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
// 自测宿主专用：把宿主**自己的岛**登记成这一次 attach 的托盘目标（也就是 T）——`--feed` 那条路要的
// 是"负载经真实 `WH_GETMESSAGE` 编组把帧渲染进宿主的岛"，所以这里换的是要做投递的那个窗口，而不是
// `taskbar_xaml_set_host_window` 那个"悬停提示属主 + 钩子确认窗口"（那是 `--mock` inline 模式的语义，
// 两种模式互斥，绝不互相顶替）。只在自测构建里存在，生产 DLL 里连这段代码都不编译。
//
// 登记之后（`hook_post_probe_armed()` 为真）`taskbar_xaml_attach` **只**认这个窗口：它不在了、不属于
// 本进程、或登记的是空窗口，attach 一律直接失败，**绝不**回退去找真实的 `Shell_TrayWnd`。
void taskbar_xaml_probe_set_tray_override(HWND window);
#endif  // DESKFEED_PAYLOAD_PROBE_BUILD

}  // namespace deskfeed::payload
