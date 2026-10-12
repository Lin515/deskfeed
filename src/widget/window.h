#pragma once

#include <windows.h>
// TOOLINFOW：行上的悬停提示是 comctl32 的 tooltip 控件，登记工具要用这个结构。
#include <commctrl.h>

#include <string>

#include "display/frame.h"

// 桌面悬浮窗：主进程里那一个窗口，只消费**已经算好的显示帧**。
//
// 责任边界（见 src/widget/README.md 与 docs/architecture.md）：
//   * 取数、解析、严格校验、缓存、到期与整体失效判断全在主进程，窗口一行都不做；
//   * 窗口不联网、不读设置文件、不绑定进程、不碰控制通道与显示邮箱、没有自己的守护线程；
//   * 窗口只把四个动作交回主进程：刷新一次、退出、切换任务栏组件、落盘界面偏好；
//   * 窗口的 HWND 建在主进程现有线程上，主进程那条 MsgWaitForMultipleObjects + PeekMessage
//     循环已经会泵它的消息，所以这里不需要新的线程或消息循环。
namespace deskfeed::widget {

// 界面偏好：与 settings.ini 的 `[window]` 一节一一对应。
//
// `taskbar_component` 对应 `[taskbar] component`，但它**不由** save_preferences 落盘：
// 切换任务栏组件走 set_taskbar_component，主进程在那个动作里自己落盘一次（不重复写）。
struct WindowPreferences {
    bool anchor_known = false;
    POINT anchor{0, 0};  // 窗口右下角（屏幕坐标）；尺寸变化时保持这个角不动
    bool expanded = false;
    bool topmost = true;
    bool hidden = false;  // 用户从菜单把悬浮窗藏起来（跨启动保持）；缺省显示
    bool taskbar_component = true;
};

// 窗口要往外做的四件事。窗口只管把动作交出去，节奏与持久化都不属于窗口。
//
// **重入与生命周期契约（硬约束）**：这些回调都是**同步**调用的，而且窗口在回调返回之后还会
// 继续用自己那份状态。所以回调实现里：
//   * 不得销毁窗口（不要调 `WidgetWindow::destroy()`，也不要 `DestroyWindow` 那个句柄）；
//   * 不得重入 `apply()` / `show_from_request()`；
//   * 不得让主进程的 `SettingsFile` 或回调上下文失效——它们必须活到
//     `WidgetWindow::destroy()` **返回之后**（收尾时还会同步调一次 `save_preferences`，
//     用于补写还没落盘的锚点）。
// 违反其中任何一条，回调返回后就是 use-after-free。
struct WidgetCallbacks {
    // 菜单「刷新」：请求主进程立刻取一次数。**不得**改地址、调度配置或绑定。
    void (*refresh)(void* context) = nullptr;
    // 菜单「退出」/ Alt+F4：正常退出。窗口自己不会重生，也不负责让主进程重生。
    // 实现里请**只投递一个退出请求**（例如 PostMessage 一条消息），不要在这个回调里同步销毁
    // 窗口：那会在窗口过程还压在栈上的时候把窗口对象拆掉。
    void (*quit)(void* context) = nullptr;
    // 菜单「任务栏组件」：把意图交给主进程，返回**切换之后**的实际状态
    // （true = 任务栏组件开着）。主进程在这个动作里落盘并驱动注入/守护。
    // 返回后窗口还会读自己的偏好与菜单句柄，所以这里同样不得销毁窗口或重入。
    bool (*set_taskbar_component)(void* context, bool enabled) = nullptr;
    // 界面偏好落盘：主进程用它**自己那一份** SettingsFile 改 `[window]` 再原子保存。
    //
    // 返回 false 表示没有落盘（设置文件不可用或写失败）；窗口会把内存里的值回滚到落盘前
    // 的那一份，保证界面与磁盘不会各说各话。窗口绝不持有 SettingsFile——若它自己持有一份
    // 旧快照，一次切换就会把刚写进去的新地址覆盖回去。
    //
    // 它会被**多次**调用（每次切换一次，收尾时还可能有一次待落盘的锚点），所以必须是廉价、
    // 无副作用的落盘动作，不能在里面做会改变窗口状态的事。
    bool (*save_preferences)(void* context, WindowPreferences const& preferences,
                             std::wstring* error) = nullptr;
    void* context = nullptr;
};

struct WidgetOptions {
    HINSTANCE instance = nullptr;
    // 窗口类名由主进程用 scoped_name 生成（隔离实例用不同类名，互不干扰）。
    std::wstring class_name;
    // --no-taskbar 时为 false：菜单里那一项置灰（没打算守护就绝不许碰目标进程）。
    bool taskbar_available = true;
    // --hidden：本次运行强制隐藏，且**不落盘**（这是运行开关，不是用户的持久意图）。
    bool force_hidden = false;
    WindowPreferences preferences{};
};

class WidgetWindow {
public:
    WidgetWindow() = default;
    ~WidgetWindow();
    WidgetWindow(WidgetWindow const&) = delete;
    WidgetWindow& operator=(WidgetWindow const&) = delete;

    // 建窗（隐藏着建）并套用落位偏好。失败时 error 说明原因。
    bool create(WidgetOptions const& options, WidgetCallbacks const& callbacks, std::wstring* error);

    HWND handle() const { return hwnd_; }

    // 主进程算好的帧：换内容、按内容定尺寸，然后决定显示还是隐藏。
    //   * ViewKind::Waiting（首次请求等待中）→ 不显示：拿到第一份快照之前不露空窗；
    //   * ViewKind::Content 且一条都没有（合法清空）→ 整窗隐藏，不是错误；
    //   * ViewKind::Content 有条目 → 按 hidden / 全屏让位规则显示；
    //   * ViewKind::Error（含“未配置 URL”）→ 显示一行红色 ERROR，即使还没有过任何快照。
    void apply(Frame const& frame);

    // 别的进程叫它出来（任务栏那块的双击或菜单）：清掉用户的隐藏意图并落盘，再按规则显示。
    void show_from_request();

    // 收尾：销毁窗口与附属的提示控件。偏好在这里最后一次落盘（兜底；每次切换都已经落过盘）。
    void destroy();

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    // 实现细节全部留在 window.cpp：这个头是主进程依赖的稳定接口，不该随内部改动而变。
    struct Impl;
    Impl* impl_ = nullptr;
    HWND hwnd_ = nullptr;
};

}  // namespace deskfeed::widget
