#pragma once

#include <windows.h>
// TOOLINFOW：行上的悬停提示是 comctl32 的 tooltip 控件，登记工具要用这个结构。
#include <commctrl.h>

#include <string>
#include <vector>

#include "display/frame.h"

// 桌面悬浮窗：主进程里那一个窗口，只消费**已经算好的显示帧**。
//
// 责任边界（见 src/widget/README.md 与 docs/architecture.md）：
//   * 取数、解析、严格校验、缓存、到期与整体失效判断全在主进程，窗口一行都不做；
//   * 窗口不联网、不读设置文件、不绑定进程、不碰控制通道与显示邮箱、没有自己的守护线程；
//   * 窗口只把动作交回主进程：刷新一次、退出、切换任务栏组件、列出/切换托盘屏幕、落盘界面偏好；
//   * 窗口的 HWND 建在主进程现有线程上，主进程那条 MsgWaitForMultipleObjects + PeekMessage
//     循环已经会泵它的消息，所以这里不需要新的线程或消息循环。
namespace deskfeed::widget {

// 通知区域图标（托盘图标）留给窗口过程的消息号。图标本身由窗口在创建时登记、销毁时撤销；
// 这条消息只承载 Shell 的鼠标/键盘事件。离线用例也按这个号码合成事件，因此不要在别处复用。
constexpr UINT kTrayIconMessage = WM_APP + 10;

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

// 悬浮窗右键菜单里的一个“托盘屏幕”条目。列表在每次弹出菜单前由主进程现查：显示器与任务栏
// 会随拔插变化，缓存在窗口里就会给出过期选项。
struct WidgetScreenEntry {
    std::wstring id;     // 稳定屏幕标识（与显示帧里的配置项同一口径）
    std::wstring label;  // 面向用户的标签，例如 `屏幕 1（主）`
    bool checked = false;
    bool available = false;  // 当前有可用的任务栏目标；不可用但已勾选的条目仍可取消
};

// 窗口要往外做的这组动作。窗口只管把动作交出去，节奏与持久化都不属于窗口。
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
    // 菜单「托盘屏幕」子菜单：每次弹出前现查一次。返回 false 或空表表示当前没有可控制的屏幕
    // （子菜单会显示一条灰色说明，不伪造条目）。实现只做枚举与读配置，不落盘、不改状态。
    bool (*list_screens)(void* context, std::vector<WidgetScreenEntry>& screens) = nullptr;
    // 勾选/取消一块屏幕。返回切换之后该项是否处于启用状态；主进程在这里落盘并让显示帧重发。
    // 与 set_taskbar_component 一样，实现不得销毁窗口或重入 apply。
    bool (*set_screen_enabled)(void* context, std::wstring const& screen_id, bool enabled) = nullptr;
    void* context = nullptr;
};

struct WidgetOptions {
    HINSTANCE instance = nullptr;
    // 窗口类名由主进程用 scoped_name 生成（隔离实例用不同类名，互不干扰）。
    std::wstring class_name;
    // --no-taskbar 时为 false：菜单里那一项置灰（没打算守护就绝不许碰目标进程）。
    bool taskbar_available = true;
    // 是否挂通知区域图标。缺省挂；离线用例设 false，避免在真实通知区域里留下图标。
    // 关掉它只影响图标，不影响窗口、菜单与右键菜单项。
    bool tray_icon_available = true;
    // --hidden：本次运行强制隐藏，且**不落盘**（这是运行开关，不是用户的持久意图）。
    // 同时不挂通知区域图标：诊断/验收运行不该在真实托盘里留下东西。
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
    // 消息的实际窗口句柄必须显式传进来：destroy() 会先把 hwnd_ 置空、再销毁窗口，而那之后
    // 窗口过程仍可能收到消息（WM_DESTROY 等），读成员会拿到 nullptr。
    LRESULT handle_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    // 实现细节全部留在 window.cpp：这个头是主进程依赖的稳定接口，不该随内部改动而变。
    struct Impl;
    Impl* impl_ = nullptr;
    HWND hwnd_ = nullptr;
};

}  // namespace deskfeed::widget
