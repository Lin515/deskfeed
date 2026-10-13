#pragma once

#include <windows.h>

#include <winrt/Windows.Foundation.h>

#include <string>
#include <vector>

// 用 XAML 诊断接口（xamlom.h，也就是 Visual Studio 的实时可视化树用的那一套）访问本进程的
// XAML 树。选这条路而不是去钩 taskbar.dll：它只依赖公开的 COM 接口，不关心某个具体版本里
// shell 的对象布局。
//
// 机制：`InitializeXamlDiagnosticsEx`（Windows.UI.Xaml.dll）会把本模块当作该进程的 "TAP" DLL
// 加载，让每一个 XAML 核心通过本模块的 `DllGetClassObject` 取一个对象，再用
// `IObjectWithSite::SetSite` 把该核心的 `IXamlDiagnostics` 交出来。所以负载与 TAP 是同一个
// 文件，不需要第二个 DLL，也不需要任何钩子框架。
//
// 拿到元素本身还要一步：shell 把 XAML 放在岛里，这种进程**没有主可视树**——
// `IXamlDiagnostics::GetUiLayer` 只给一个空的合成根，`HitTest` 在没有 XamlRoot 句柄时直接
// 返回 `E_INVALIDARG`。能用的是 `AdviseVisualTreeChange`：它把该核心**现有整棵树**逐个上报
// （含 Type 与 Name），这些函数驱动的就是那次走查。
namespace deskfeed::payload {

// 一个已连接上的 XAML 核心。
struct XamlTapSite {
    // 核心调用 SetSite 的那条线程，也就是拥有它那棵 XAML 树的线程。
    unsigned long thread_id = 0;
    // 该核心的 IXamlDiagnostics。本模块持有一个引用。
    void* diagnostics = nullptr;
};

// 连接本进程的 XAML 核心。每个核心在自己的线程上应答，所以这里只是**发起**连接：
// 稍后再调 xaml_tap_sites() 看谁回来了。
bool xaml_tap_start(std::wstring& error);

// 到目前为止连上的站点，按连接顺序。
std::vector<XamlTapSite> xaml_tap_sites();

// 等**本轮应答过的端口**把站点交出来，最多等 deadline_ms；到限就带着现有站点返回。
//
// 为什么需要它：端口应答是异步的，每个核心在自己的线程上稍后才调用 SetSite。只等"有一个站点
// 就行"是过早的——其余核心的 SetSite 这时还在路上，而走查一开始、`xaml_tap_stop` 一跑，
// `stopping` 就置位，晚到的站点被拒收，那个端口取走的模块引用就留在 loader 账上。
//
// 返回值只是现有的站点数：到限时不按端口数补引用（归还规则见 xaml_tap_stop），也不改变任何
// 引用归属。没连接时调用安全，返回 0。
size_t xaml_tap_wait_for_sites(size_t deadline_ms);

// 走查一个核心的树一次，记下叫这个名字的元素的句柄（如果树里有）。此时还没有解析任何东西，
// 上报过的句柄也还没有还回去。
//
// 必须在**不是核心自己线程**的地方调用：走查会被编组到核心线程并等它执行完，在核心线程上调用
// 就是死锁。
//
// **一次注入只调用一次。** 诊断会为走查在进程里另外起一套 composition 走查器
// （CreateCompVisualDiag），在任务栏那条线程上反复走查是本项目认定的最高风险操作；所以元素
// 解析出来之后运行期再也不走查（taskbar_xaml.cpp 在解析阶段就把连接关掉，之后这里连站点都
// 找不到）。
bool xaml_tap_watch(size_t index, std::wstring const& element_name, std::wstring& error);

// 解析 xaml_tap_watch 找到的东西。必须在核心自己的线程上调用——它返回的对象属于那条线程。
bool xaml_tap_resolve(size_t index, winrt::Windows::Foundation::IInspectable& element,
                      std::wstring& error);

// 把走查上报过的每一个句柄还回去。在它跑之前，诊断一直持有每个上报元素的一份引用。
void xaml_tap_release_walk(size_t index);

// 释放所有站点，并给出**真实证据**：站点引用按实际交出的核心数归还，且 SetSite 已经排空到零。
// 返回 false 表示有 SetSite 在期限内没有返回（那一刻不宣称退休），detail 写明原因。
// 没有连接时调用是安全的，返回 true；重复调用同样安全。
bool xaml_tap_stop(unsigned timeout_ms, std::wstring& detail);

// TAP 的类对象，由 XAML 经本模块的 DllGetClassObject 调用。
HRESULT tap_get_class_object(REFCLSID class_id, REFIID interface_id, void** object);

}  // namespace deskfeed::payload
