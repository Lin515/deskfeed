// 悬浮窗的**隐藏窗口门禁**：在测试进程自己的线程上建一个真窗口，但用 `force_hidden = true`
// 让它永远不显示，所以真实悬浮窗绝不会出现在桌面上。
//
// 这里只发**合成窗口消息**与直接调用公开方法：不移动光标、不弹可见菜单、不注入 explorer、
// 不碰用户正在跑的实例。悬停提示本身就是由窗口消息驱动的（命中判定在窗口里，显示走
// `TTM_TRACKPOSITION` + `TTM_TRACKACTIVATE`），所以合成 `WM_MOUSEMOVE` 走的就是生产路径，
// 提示的可见性、文本与摆放位置都能在这里断言。不在这里覆盖的是需要真实桌面的行为
// （拖动落盘后的真机位置、真实指针是否命中、全屏让位）。
//
// `WM_COMMAND` 的命令号是窗口对外的稳定接口（菜单项与离线夹具共用），这里按号发送。

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <objidl.h>
#include <gdiplus.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/utf8.h"
#include "display/frame.h"
#include "display/reading.h"
#include "platform/log.h"
#include "platform/win32_util.h"
#include "widget/fullscreen.h"
#include "widget/text_metrics.h"
#include "widget/theme.h"
#include "widget/window.h"

namespace {

int g_failures = 0;

void check(bool condition, std::string const& name) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", name.c_str());
    }
}

// window.cpp 里定义的菜单命令号（对外稳定接口）。
constexpr UINT kCmdRefresh = 1;
constexpr UINT kCmdExpanded = 2;
constexpr UINT kCmdTaskbarComponent = 3;
constexpr UINT kCmdHide = 4;
constexpr UINT kCmdExit = 5;
constexpr UINT kCmdTopmost = 6;

constexpr wchar_t kTestClass[] = L"DeskFeedWidgetGateWindow";

// 记录回调收到的动作。回调必须只做这些事：改自己的计数器、返回结果——不销毁窗口、不重入。
struct Recorder {
    int refresh = 0;
    int quit = 0;
    int taskbar_calls = 0;
    int saves = 0;
    // 落盘里出现过 hidden=true：这一条就是“隐藏悬浮窗那条路被走过”的证据。
    int hidden_saves = 0;
    bool save_ok = true;
    bool taskbar_result = true;
    deskfeed::widget::WindowPreferences last_saved{};
};

void on_refresh(void* context) { static_cast<Recorder*>(context)->refresh += 1; }
void on_quit(void* context) { static_cast<Recorder*>(context)->quit += 1; }
bool on_taskbar(void* context, bool enabled) {
    Recorder* recorder = static_cast<Recorder*>(context);
    recorder->taskbar_calls += 1;
    recorder->taskbar_result = enabled;
    return enabled;
}
bool on_save(void* context, deskfeed::widget::WindowPreferences const& preferences,
             std::wstring* error) {
    Recorder* recorder = static_cast<Recorder*>(context);
    recorder->saves += 1;
    recorder->last_saved = preferences;
    if (preferences.hidden) {
        recorder->hidden_saves += 1;
    }
    if (!recorder->save_ok) {
        if (error != nullptr) {
            *error = L"injected save failure";
        }
        return false;
    }
    return true;
}

deskfeed::widget::WidgetCallbacks callbacks_for(Recorder& recorder) {
    deskfeed::widget::WidgetCallbacks callbacks;
    callbacks.refresh = &on_refresh;
    callbacks.quit = &on_quit;
    callbacks.set_taskbar_component = &on_taskbar;
    callbacks.save_preferences = &on_save;
    callbacks.context = &recorder;
    return callbacks;
}

deskfeed::Frame content_frame(std::size_t items) {
    deskfeed::Frame frame;
    frame.kind = deskfeed::ViewKind::Content;
    for (std::size_t index = 0; index < items; ++index) {
        deskfeed::FrameItem item;
        item.id = "i" + std::to_string(index);
        item.label = "cpu";
        item.value = std::to_string(index) + "C";
        frame.items.push_back(std::move(item));
    }
    return frame;
}

// 与生产同形：每条目带非空 detail（生产快照里 4 个条目的 detail 长度在 21..115 之间）。
deskfeed::Frame detailed_content_frame(std::size_t items) {
    deskfeed::Frame frame = content_frame(items);
    for (std::size_t index = 0; index < frame.items.size(); ++index) {
        frame.items[index].detail = "detail of item " + std::to_string(index);
    }
    return frame;
}

bool is_topmost(HWND window) {
    return (::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

SIZE window_size(HWND window) {
    RECT rect{};
    ::GetWindowRect(window, &rect);
    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
}

// 悬停提示是一个 WS_POPUP 的**被拥有**窗口（不是子窗口），所以 FindWindowEx 找不到它：
// 按类名在本线程的窗口里找、并核对属主。
struct TipSearch {
    HWND owner;
    HWND found;
};

BOOL CALLBACK tip_search_proc(HWND window, LPARAM param) {
    auto* state = reinterpret_cast<TipSearch*>(param);
    wchar_t class_name[64] = {};
    ::GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
    if (_wcsicmp(class_name, TOOLTIPS_CLASSW) == 0 &&
        ::GetWindow(window, GW_OWNER) == state->owner) {
        state->found = window;
        return FALSE;
    }
    return TRUE;
}

HWND find_tooltip(HWND owner) {
    TipSearch state{owner, nullptr};
    ::EnumThreadWindows(::GetCurrentThreadId(), &tip_search_proc,
                        reinterpret_cast<LPARAM>(&state));
    return state.found;
}

// 建一个永远不会显示的悬浮窗。
bool create_hidden(deskfeed::widget::WidgetWindow& window, Recorder& recorder,
                   deskfeed::widget::WindowPreferences preferences, std::wstring& error) {
    deskfeed::widget::WidgetOptions options;
    options.instance = ::GetModuleHandleW(nullptr);
    options.class_name = kTestClass;
    options.taskbar_available = true;
    options.force_hidden = true;  // 关键：整轮门禁都不会显示真实窗口
    // 用例不登记真实通知区域图标：Shell 的图标属于当前桌面，离线门禁不该在用户托盘里留东西。
    options.tray_icon_available = false;
    options.preferences = preferences;
    return window.create(options, callbacks_for(recorder), &error);
}

// ---- 悬停与双击的公共夹具 --------------------------------------------------------------------

void pump_for(unsigned ms) {
    unsigned long long const deadline = ::GetTickCount64() + ms;
    MSG message{};
    while (::GetTickCount64() < deadline) {
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        ::Sleep(5);
    }
}

WNDPROC window_proc_of(HWND window) {
    return reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(window, GWLP_WNDPROC));
}

// comctl32 现在为悬停持有**一个**工具（uId 固定为 0），文本按命中的条目换。
constexpr UINT kHoverToolId = 0;

std::wstring tool_text_of(HWND tip, HWND owner, UINT id) {
    wchar_t buffer[2048] = {};
    TOOLINFOW tool{};
    tool.cbSize = sizeof(TOOLINFOW);
    tool.hwnd = owner;
    tool.uId = id;
    tool.lpszText = buffer;
    ::SendMessageW(tip, TTM_GETTEXTW, static_cast<WPARAM>(std::size(buffer)),
                   reinterpret_cast<LPARAM>(&tool));
    return buffer;
}

// 提示当下显示的那一行文本；提示没显示时就是空。
std::wstring shown_hover_text(HWND tip, HWND owner) {
    if (tip == nullptr || !::IsWindowVisible(tip)) {
        return std::wstring();
    }
    return tool_text_of(tip, owner, kHoverToolId);
}

// 帧里每个条目自己的提示文本，按排版顺序（与任务栏那侧悬停格同一个字段 `ReadingRow::tip`）。
std::vector<std::wstring> expected_node_tips(deskfeed::Frame const& frame) {
    deskfeed::ReadingContent const content = deskfeed::reading_from_frame(frame);
    std::vector<std::wstring> tips;
    for (deskfeed::ReadingRow const& row : content.rows) {
        tips.push_back(deskfeed::wide_from_utf8(row.tip));
    }
    return tips;
}

// 在整个客户区上合成鼠标移动，返回“提示文本的变化序列”（空 = 提示没显示）。
std::vector<std::wstring> sweep_hover_texts(HWND hwnd, HWND tip, int step = 2) {
    RECT client{};
    if (!::GetClientRect(hwnd, &client)) {
        return {};
    }
    std::vector<std::wstring> seen;
    std::wstring last = L"<start>";
    for (int y = 1; y < client.bottom; y += step) {
        for (int x = 1; x < client.right; x += step) {
            ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
            std::wstring const text = shown_hover_text(tip, hwnd);
            if (text != last) {
                seen.push_back(text);
                last = text;
            }
        }
    }
    return seen;
}

std::vector<std::wstring> non_empty(std::vector<std::wstring> const& values) {
    std::vector<std::wstring> kept;
    for (std::wstring const& value : values) {
        if (!value.empty()) {
            kept.push_back(value);
        }
    }
    return kept;
}

// 同一行里从左到右扫，每一行都会重看一遍同样的节点；这里只留**第一次出现**的顺序。
std::vector<std::wstring> unique_in_order(std::vector<std::wstring> const& values) {
    std::vector<std::wstring> kept;
    for (std::wstring const& value : values) {
        bool known = false;
        for (std::wstring const& seen : kept) {
            if (seen == value) {
                known = true;
                break;
            }
        }
        if (!known) {
            kept.push_back(value);
        }
    }
    return kept;
}

int gdi_objects() { return static_cast<int>(::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS)); }
int user_objects() {
    return static_cast<int>(::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS));
}

// 找到一个能悬停出 `text` 的客户区点；指针不动也能从合成消息里挑出来。
bool find_hover_point(HWND hwnd, HWND tip, std::wstring const& text, POINT* out) {
    RECT client{};
    if (!::GetClientRect(hwnd, &client)) {
        return false;
    }
    for (int y = 1; y < client.bottom; y += 2) {
        for (int x = 1; x < client.right; x += 2) {
            ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
            if (shown_hover_text(tip, hwnd) == text) {
                *out = POINT{x, y};
                return true;
            }
        }
    }
    return false;
}

// 逐条指定 value/detail 的帧：用来只改排版宽度（value）或只改提示文本（detail）。
deskfeed::Frame hover_frame(std::vector<std::string> const& values,
                            std::vector<std::string> const& details) {
    deskfeed::Frame frame;
    frame.kind = deskfeed::ViewKind::Content;
    for (std::size_t index = 0; index < values.size(); ++index) {
        deskfeed::FrameItem item;
        item.id = "i" + std::to_string(index);
        item.label = "cpu";
        item.value = values[index];
        item.detail = index < details.size() ? details[index] : std::string();
        frame.items.push_back(std::move(item));
    }
    return frame;
}

bool same_rect(RECT const& left, RECT const& right) {
    return left.left == right.left && left.top == right.top && left.right == right.right &&
           left.bottom == right.bottom;
}

// ---- 悬停：每个条目自己的提示，稳定、可断言 --------------------------------------------------

// 悬停必须按**条目**给出提示（与任务栏那侧同一个字段），不是把整行拼成一条；
// 同一个条目里指针怎么动都不重发；离开条目/窗口/整窗隐藏时提示必须收掉。
void test_hover_shows_each_nodes_own_tip() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    check(create_hidden(window, recorder, preferences, error), "create for the hover gate");
    HWND const hwnd = window.handle();
    deskfeed::Frame const frame = detailed_content_frame(4);

    // 我们去掉了 TTF_SUBCLASS：comctl32 不该再替换本窗口的窗口过程。
    WNDPROC const proc_before = window_proc_of(hwnd);
    window.apply(frame);
    check(window_proc_of(hwnd) == proc_before,
          "comctl32 never replaces the widget window procedure (no TTF_SUBCLASS)");

    HWND const tip = find_tooltip(hwnd);
    check(tip != nullptr, "the hover tooltip control exists");
    if (tip == nullptr) {
        window.destroy();
        return;
    }
    check(::SendMessageW(tip, TTM_GETTOOLCOUNT, 0, 0) == 1, "exactly one hover tool is registered");

    std::vector<std::wstring> const expected = expected_node_tips(frame);
    check(expected.size() == 4, "the frame has four hoverable nodes");

    std::vector<std::wstring> const tips = unique_in_order(non_empty(sweep_hover_texts(hwnd, tip)));
    check(tips == expected,
          "sweeping the window shows every node's own tip, in packing order (not the whole row)");

    // 同一个条目里再动几下：文本与提示位置都不该变（提示钉在节点上，不跟鼠标走）。
    if (!expected.empty()) {
        RECT client{};
        ::GetClientRect(hwnd, &client);
        POINT inside{0, 0};
        for (int y = 1; y < client.bottom && inside.x == 0; y += 2) {
            for (int x = 1; x < client.right; x += 2) {
                ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
                if (shown_hover_text(tip, hwnd) == expected[0]) {
                    inside = POINT{x, y};
                    break;
                }
            }
        }
        check(inside.x != 0, "the first node is reachable by a synthetic move");
        RECT tip_before{};
        ::GetWindowRect(tip, &tip_before);
        ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(inside.x + 1, inside.y));
        ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(inside.x + 2, inside.y));
        RECT tip_after{};
        ::GetWindowRect(tip, &tip_after);
        check(shown_hover_text(tip, hwnd) == expected[0], "moving inside one node keeps its tip");
        check(tip_before.left == tip_after.left && tip_before.top == tip_after.top,
              "the tip does not follow the pointer inside one node");

        // 内容不变的一次刷新不该把正在看的提示弄没。
        window.apply(frame);
        check(shown_hover_text(tip, hwnd) == expected[0],
              "a refresh with unchanged content keeps the tip on the same node");

        // 提示必须落在节点所在显示器的工作区里（否则就是“弹到屏幕外，看不见”）。
        RECT tip_rect{};
        ::GetWindowRect(tip, &tip_rect);
        HMONITOR const monitor = ::MonitorFromRect(&tip_rect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        bool inside_work = false;
        if (monitor != nullptr && ::GetMonitorInfoW(monitor, &info)) {
            inside_work = tip_rect.left >= info.rcWork.left && tip_rect.top >= info.rcWork.top &&
                          tip_rect.right <= info.rcWork.right &&
                          tip_rect.bottom <= info.rcWork.bottom;
        }
        check(inside_work, "the tip is placed inside the monitor work area");
    }

    // 行外的空白处（左上角内边距）不该冒提示。
    ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(2, 2));
    check(!::IsWindowVisible(tip), "the padding around the rows shows no tip");

    // 合成一条 WM_MOUSELEAVE：提示必须收掉。
    ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10));
    ::SendMessageW(hwnd, WM_MOUSELEAVE, 0, 0);
    check(!::IsWindowVisible(tip), "leaving the window hides the tip");

    // 内容清空：窗口隐藏，提示也必须跟着收掉。
    deskfeed::Frame empty;
    empty.kind = deskfeed::ViewKind::Content;
    ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10));
    window.apply(empty);
    check(!::IsWindowVisible(hwnd), "an empty frame hides the window");
    check(!::IsWindowVisible(tip), "an empty frame hides the hover tip too");

    window.destroy();
}

// 详细模式下提示同样只认条目：折行之外多出来的详情块不该变成新的悬停节点。
void test_hover_in_detailed_mode_covers_only_nodes() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    check(create_hidden(window, recorder, preferences, error), "create for the detailed hover gate");
    HWND const hwnd = window.handle();
    deskfeed::Frame const frame = detailed_content_frame(4);
    window.apply(frame);
    HWND const tip = find_tooltip(hwnd);
    if (tip == nullptr) {
        check(false, "the tooltip exists in detailed mode");
        window.destroy();
        return;
    }
    ::SendMessageW(hwnd, WM_COMMAND, kCmdExpanded, 0);
    check(recorder.last_saved.expanded, "the detailed mode switch persisted");
    std::vector<std::wstring> const tips = unique_in_order(non_empty(sweep_hover_texts(hwnd, tip)));
    std::vector<std::wstring> const expected = expected_node_tips(frame);
    check(tips == expected, "detailed mode still hovers per node, and only the nodes");
    window.destroy();
}

// 已经显示出来的提示要经得起“指针没动、内容刷新了”：
//   * 同一条目、同一份提示文本，但这一帧把排版宽度改了、条目矩形挪了 → 提示必须**还在**，
//     而且位置一个像素都不动（旧写法的“矩形变了就收掉、等鼠标再显示”会在这里闪一下）；
//   * 同一条目换了提示文本 → 只换文本：位置不动，也不必等下一次鼠标移动；
//   * 悬停的那个条目在下一帧里没有了 / 内容清空 → 必须收掉。
void test_hover_same_node_survives_refresh() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    check(create_hidden(window, recorder, preferences, error), "create for the hover refresh gate");
    HWND const hwnd = window.handle();
    std::vector<std::string> values{"1C", "2C", "3C", "4C"};
    std::vector<std::string> details{"detail 0", "detail 1", "detail 2", "detail 3"};
    window.apply(hover_frame(values, details));
    HWND const tip = find_tooltip(hwnd);
    if (tip == nullptr) {
        check(false, "the refresh gate has a tooltip control");
        window.destroy();
        return;
    }
    std::vector<std::wstring> const expected = expected_node_tips(hover_frame(values, details));

    // 指针停在第一个条目上。
    POINT at{0, 0};
    check(find_hover_point(hwnd, tip, expected[0], &at), "the first node was reachable");
    ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(at.x, at.y));
    RECT before{};
    ::GetWindowRect(tip, &before);
    check(::IsWindowVisible(tip), "the tip is visible before the refresh");

    // (1) 同文本、矩形变：把数值拉宽，排版跟着变。指针一个消息都不发。
    values[0] = "123456789C";
    window.apply(hover_frame(values, details));
    RECT after_geometry{};
    ::GetWindowRect(tip, &after_geometry);
    check(::IsWindowVisible(tip), "a refresh that only moves the layout keeps the tip visible");
    check(same_rect(before, after_geometry),
          "a refresh that only moves the layout does not move the tip");
    check(shown_hover_text(tip, hwnd) == expected[0], "the moved layout keeps the same tip text");

    // (2) 同条目、换文本：指针仍然不动。
    details[0] = "detail 0 changed";
    window.apply(hover_frame(values, details));
    RECT after_text{};
    ::GetWindowRect(tip, &after_text);
    check(::IsWindowVisible(tip), "a new tip text keeps the tip visible");
    check(shown_hover_text(tip, hwnd) == deskfeed::wide_from_utf8("detail 0 changed"),
          "the new tip text shows up without waiting for the pointer to move");
    check(after_text.left == after_geometry.left && after_text.top == after_geometry.top,
          "the new tip text keeps the tip where it was");

    // 换文本之后整窗扫一遍：其它条目还是各自的文本，第一个条目是新文本。
    std::vector<std::wstring> const expected_after = expected_node_tips(hover_frame(values, details));
    std::vector<std::wstring> const tips = unique_in_order(non_empty(sweep_hover_texts(hwnd, tip)));
    check(tips == expected_after, "after a text-only refresh every node still hovers its own text");

    // (3) 悬停的条目在下一帧里没有了：只留两条时第二个视觉行整个消失。
    POINT last{0, 0};
    check(find_hover_point(hwnd, tip, expected_after[3], &last), "the fourth node was reachable");
    ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(last.x, last.y));
    check(::IsWindowVisible(tip), "the fourth node shows a tip");
    std::vector<std::string> const fewer_values{"1C", "2C"};
    std::vector<std::string> const fewer_details{"detail 0 changed", "detail 1"};
    window.apply(hover_frame(fewer_values, fewer_details));
    check(!::IsWindowVisible(tip), "the hovered node disappearing hides the tip");

    // (4) 内容清空：一样必须收掉。
    POINT first{0, 0};
    check(find_hover_point(hwnd, tip, deskfeed::wide_from_utf8("detail 0 changed"), &first),
          "the first node is reachable again");
    ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(first.x, first.y));
    check(::IsWindowVisible(tip), "the first node shows a tip again");
    deskfeed::Frame empty;
    empty.kind = deskfeed::ViewKind::Content;
    window.apply(empty);
    check(!::IsWindowVisible(tip), "an empty frame hides the tip of the displayed node");
    window.destroy();
}

// ---- 双击：真实消息序列必须切换详细/精简，并且立刻落盘 --------------------------------

void test_double_click_toggles_and_persists() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    check(create_hidden(window, recorder, preferences, error), "create for the double-click gate");
    HWND const hwnd = window.handle();
    window.apply(detailed_content_frame(2));

    // 类带 CS_DBLCLKS：真实双击的第二次按下是 WM_LBUTTONDBLCLK，**不是** WM_LBUTTONDOWN。
    check((::GetClassLongPtrW(hwnd, GCL_STYLE) & CS_DBLCLKS) != 0,
          "the widget class asks the system for double-click messages");

    int const saves_before = recorder.saves;
    auto double_click = [hwnd] {
        ::SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 10));
        ::SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(20, 10));
        ::SendMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(20, 10));
        ::SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(20, 10));
    };
    double_click();
    check(recorder.saves == saves_before + 1, "a real double click persists the switch once");
    check(recorder.last_saved.expanded, "a real double click switches to the detailed mode");
    double_click();
    check(recorder.saves == saves_before + 2, "the second double click persists once");
    check(!recorder.last_saved.expanded, "the second double click switches back to compact");
    // 双击留下的尾巴不能算成一次新的点按：紧接着的一次单click不该切模式。
    ::SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 10));
    ::SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(20, 10));
    check(recorder.saves == saves_before + 2, "a single click right after a double click is not one");
    // 两次抬起很近、但系统没发 DBLCLK 的那条兜底路径也要能切一次（只切一次）。
    ::SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 12));
    ::SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(30, 12));
    ::SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 12));
    ::SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(30, 12));
    check(recorder.saves == saves_before + 3, "the two-tap fallback still switches exactly once");
    check(recorder.last_saved.expanded, "the fallback switched the mode");

    window.destroy();
}

// 拖动不能误判成双击：真正移动过的那一次按下，收尾之后系统仍可能补一条 DBLCLK。
void test_drag_does_not_toggle_the_mode() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    preferences.anchor_known = true;
    preferences.anchor = POINT{900, 700};
    check(create_hidden(window, recorder, preferences, error), "create for the drag gate");
    HWND const hwnd = window.handle();
    window.apply(detailed_content_frame(2));
    int const saves_before = recorder.saves;
    LONG const height_before = window_size(hwnd).cy;

    ::SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 10));
    ::SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(80, 70));
    ::ReleaseCapture();  // 拖动收尾
    // 拖动收尾之后系统补的那一条 DBLCLK 不该切模式。
    ::SendMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(20, 10));
    ::SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(20, 10));
    check(recorder.saves == saves_before + 1, "the drag persisted the anchor exactly once");
    check(!recorder.last_saved.expanded, "the drag did not switch the mode");
    check(window_size(hwnd).cy == height_before, "the drag did not resize the window either");
    window.destroy();
}

// ---- 压力循环：详细↔精简、取数刷新、悬停显示与定时器在同一轮里跑 --------------------------------

// 不只断言“没崩”：每一轮都要核对待显示的条目与提示内容一致、句柄没被重建、
// GDI/USER 对象不增长、提示不会留在屏幕上。
void test_hover_and_detail_stress_keeps_state_consistent() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    check(create_hidden(window, recorder, preferences, error), "create for the stress gate");
    HWND const hwnd = window.handle();
    HWND const tip = find_tooltip(hwnd);
    check(tip != nullptr, "the stress gate has a tooltip control");
    if (tip == nullptr) {
        window.destroy();
        return;
    }

    int const gdi_before = gdi_objects();
    int const user_before = user_objects();
    bool nodes_always_match = true;
    bool hidden_when_empty = true;
    bool handles_stable = true;
    // 60 轮只是把同一份证据重复三遍：每轮都已经走完整的 apply + 悬停扫查，24 轮足以覆盖
    // 详细/精简切换、空帧与定时器的各种交错。
    int const rounds = 24;
    for (int round = 0; round < rounds; ++round) {
        std::size_t const items = static_cast<std::size_t>(round % 6) + 1;
        deskfeed::Frame const frame = detailed_content_frame(items);
        window.apply(frame);
        if (round % 2 == 0) {
            ::SendMessageW(hwnd, WM_COMMAND, kCmdExpanded, 0);
        }
        // 一次真实取数不会改变内容：再一次 apply 不该把提示弄丢。
        window.apply(frame);
        std::vector<std::wstring> const expected = expected_node_tips(frame);
        std::vector<std::wstring> const tips =
            unique_in_order(non_empty(sweep_hover_texts(hwnd, tip, 4)));
        if (tips != expected) {
            nodes_always_match = false;
        }
        if (::GetWindow(hwnd, GW_OWNER) != tip && !::IsWindow(tip)) {
            handles_stable = false;
        }
        if (round % 5 == 0) {
            deskfeed::Frame empty;
            empty.kind = deskfeed::ViewKind::Content;
            window.apply(empty);
            ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10));
            if (::IsWindowVisible(tip)) {
                hidden_when_empty = false;
            }
        }
        if (round % 4 == 3) {
            pump_for(5);  // 让置顶守护/全屏判定的定时器也跑几轮
        }
    }
    check(::IsWindow(hwnd), "the window survives the detail/hover stress loop");
    check(::IsWindow(tip), "the tooltip control survives the stress loop");
    check(window.handle() == hwnd, "the window handle was never recreated");
    check(nodes_always_match, "every round hovers exactly the frame's own nodes");
    check(hidden_when_empty, "an empty frame never leaves a tip on the screen");
    check(handles_stable, "the tooltip handle stays alive for the whole loop");
    int const gdi_after = gdi_objects();
    int const user_after = user_objects();
    check(gdi_after <= gdi_before + 4,
          "the stress loop does not grow the GDI object count (" + std::to_string(gdi_before) +
              " -> " + std::to_string(gdi_after) + ")");
    check(user_after <= user_before + 4,
          "the stress loop does not grow the USER object count (" + std::to_string(user_before) +
              " -> " + std::to_string(user_after) + ")");
    window.destroy();
    check(!::IsWindow(tip), "destroying the widget destroys its tooltip");
}

// ---- 门禁 ----------------------------------------------------------------------------------

void test_create_is_hidden_and_tooltips_are_configured() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;  // 缺省：显示意图，但 force_hidden 压着
    check(create_hidden(window, recorder, preferences, error), "create a hidden widget window");
    check(window.handle() != nullptr, "handle is valid after create");
    check(!::IsWindowVisible(window.handle()), "force_hidden keeps the real window hidden");

    HWND const tip = find_tooltip(window.handle());
    check(tip != nullptr, "the tooltip control exists");
    if (tip != nullptr) {
        check(::SendMessageW(tip, TTM_GETMAXTIPWIDTH, 0, 0) == 420, "tooltip wrap width is 420");
    }

    window.apply(content_frame(3));
    check(!::IsWindowVisible(window.handle()), "content does not reveal a force-hidden window");
    if (tip != nullptr) {
        check(::SendMessageW(tip, TTM_GETTOOLCOUNT, 0, 0) == 1, "exactly one hover tool");
        // 登记用的 cbSize 按候选试：至少有一个能被这一版 comctl32 接受。
        UINT const candidates[] = {sizeof(TOOLINFOW), TTTOOLINFOW_V2_SIZE,
                                   static_cast<UINT>(offsetof(TOOLINFOW, lParam))};
        bool accepted = false;
        for (UINT const candidate : candidates) {
            TOOLINFOW tool{};
            tool.cbSize = candidate;
            tool.hwnd = window.handle();
            tool.uId = 0;
            if (::SendMessageW(tip, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool)) != FALSE) {
                accepted = true;
                break;
            }
        }
        check(accepted, "the tooltip control accepts a compatible TOOLINFOW cbSize");
    }

    // 没有任何切换：一次偏好都不该写。
    check(recorder.saves == 0, "a force-hidden window writes no preference by itself");
    window.destroy();
    check(window.handle() == nullptr, "handle is cleared after destroy");
}

void test_menu_toggles_callbacks_rollback_and_refresh() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    preferences.topmost = true;
    check(create_hidden(window, recorder, preferences, error), "create for the menu gate");
    HWND const hwnd = window.handle();
    window.apply(content_frame(3));

    // 置顶开关：回调立刻收到新值，窗口的系统状态跟着走。
    ::SendMessageW(hwnd, WM_COMMAND, kCmdTopmost, 0);
    check(recorder.saves == 1, "the topmost toggle persists immediately");
    check(!recorder.last_saved.topmost, "the persisted value is the new one");
    check(!is_topmost(hwnd), "the window really lost the topmost bit");
    ::SendMessageW(hwnd, WM_COMMAND, kCmdTopmost, 0);
    check(recorder.saves == 2, "the second topmost toggle persists too");
    check(recorder.last_saved.topmost, "the second toggle restores the value");
    check(is_topmost(hwnd), "the window really has the topmost bit again");

    // 落盘失败必须回滚：窗口的系统状态与内存里的偏好都回到落盘前那一份。
    recorder.save_ok = false;
    int const saves_before_failure = recorder.saves;
    ::SendMessageW(hwnd, WM_COMMAND, kCmdTopmost, 0);
    check(recorder.saves == saves_before_failure + 1, "a failing toggle still attempts the write");
    check(is_topmost(hwnd), "a failing toggle rolls the window state back");
    recorder.save_ok = true;

    // 详细/精简：切换即落盘，尺寸跟着变。
    SIZE const compact = window_size(hwnd);
    ::SendMessageW(hwnd, WM_COMMAND, kCmdExpanded, 0);
    check(recorder.last_saved.expanded, "the expanded toggle persists immediately");
    SIZE const expanded = window_size(hwnd);
    check(expanded.cy > compact.cy, "the expanded window is taller than the compact one");
    ::SendMessageW(hwnd, WM_COMMAND, kCmdExpanded, 0);
    check(!recorder.last_saved.expanded, "the second expanded toggle restores the value");

    // 任务栏组件：窗口只递意图、按返回的实际状态打勾，不自己落盘（主进程落一次）。
    int const saves_before_taskbar = recorder.saves;
    ::SendMessageW(hwnd, WM_COMMAND, kCmdTaskbarComponent, 0);
    check(recorder.taskbar_calls == 1, "the taskbar switch asks the master exactly once");
    check(recorder.saves == saves_before_taskbar,
          "the taskbar switch does not persist through save_preferences");

    // 刷新：只请求一次取数。
    ::SendMessageW(hwnd, WM_COMMAND, kCmdRefresh, 0);
    check(recorder.refresh == 1, "refresh asks the master for one fetch");

    // 退出：只通知，窗口必须还在（契约要求 quit 不得同步销毁窗口）。
    ::SendMessageW(hwnd, WM_COMMAND, kCmdExit, 0);
    check(recorder.quit == 1, "exit notifies the master");
    check(::IsWindow(hwnd), "the window survives the quit callback (it only posts a request)");

    window.destroy();
}

void test_show_request_leaves_disk_alone_while_force_hidden() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    preferences.hidden = true;  // 用户存下来的“藏着”
    check(create_hidden(window, recorder, preferences, error), "create with a saved hidden flag");
    window.apply(content_frame(2));

    int const saves_before = recorder.saves;
    window.show_from_request();
    check(recorder.saves == saves_before,
          "--hidden: a show request does not rewrite the saved hidden flag");
    check(!::IsWindowVisible(window.handle()), "the window still never shows");

    // 托盘双击走的是同一条显示路径，同样不能绕过 --hidden。
    ::SendMessageW(window.handle(), deskfeed::widget::kTrayIconMessage, 0,
                   MAKELPARAM(WM_LBUTTONDBLCLK, 1));
    check(recorder.saves == saves_before,
          "--hidden: a tray double-click does not rewrite the saved hidden flag");
    check(!::IsWindowVisible(window.handle()), "--hidden: a tray double-click still never shows");
    window.destroy();
}

// 通知区域图标的契约：左键单击什么都不做；左键双击走“叫出悬浮窗”的那条路。
// 这一轮不 force_hidden，但在调用双击前没有内容（没有 apply 帧），所以显示路径会停在
// “拿到第一份内容再显示”，窗口全程不可见——门禁仍然是隐藏窗口门禁。
void test_tray_single_click_is_inert_and_double_click_requests_show() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    preferences.hidden = true;  // 用户存下来的“藏着”：双击应该把它清掉并落盘
    deskfeed::widget::WidgetOptions options;
    options.instance = ::GetModuleHandleW(nullptr);
    options.class_name = kTestClass;
    options.taskbar_available = true;
    options.force_hidden = false;
    options.tray_icon_available = false;
    options.preferences = preferences;
    check(window.create(options, callbacks_for(recorder), &error), "create for the tray gate");
    HWND const hwnd = window.handle();
    int const saves_before = recorder.saves;

    // 单击：v4 会送 NIN_SELECT + WM_LBUTTONUP，旧版只有 WM_LBUTTONUP。两条都不触发任何动作。
    ::SendMessageW(hwnd, deskfeed::widget::kTrayIconMessage, 0, MAKELPARAM(NIN_SELECT, 1));
    ::SendMessageW(hwnd, deskfeed::widget::kTrayIconMessage, 0, MAKELPARAM(WM_LBUTTONUP, 1));
    check(recorder.saves == saves_before, "a single tray click writes no preference");
    check(!::IsWindowVisible(hwnd), "a single tray click does not reveal the window");

    // 双击：只走“显示请求”，清掉隐藏标记；没有内容，所以窗口仍然不出现。
    ::SendMessageW(hwnd, deskfeed::widget::kTrayIconMessage, 0, MAKELPARAM(WM_LBUTTONDBLCLK, 1));
    check(recorder.saves == saves_before + 1, "a tray double-click takes the show path once");
    check(!recorder.last_saved.hidden, "a tray double-click clears the saved hidden flag");
    check(!::IsWindowVisible(hwnd), "without content the window still does not appear");
    window.destroy();
}

void test_anchor_is_not_rewritten_by_content_churn() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    preferences.anchor_known = true;
    preferences.anchor = POINT{1200, 900};
    check(create_hidden(window, recorder, preferences, error), "create with a known anchor");
    HWND const hwnd = window.handle();

    window.apply(content_frame(1));
    SIZE const size_one = window_size(hwnd);
    window.apply(content_frame(5));
    SIZE const size_five = window_size(hwnd);
    check(size_five.cx >= size_one.cx && size_five.cy > size_one.cy,
          "the window resizes with its content");
    // 隐藏期间不落位，也就不会把锚点按中间尺寸改写：一次偏好都没写。
    check(recorder.saves == 0, "content churn while hidden does not rewrite the anchor");
    window.destroy();
    // 收尾只补“还欠着的那一次”，这里没有欠账，所以仍然没有写。
    check(recorder.saves == 0, "destroy does not rewrite unchanged preferences");
}

void test_external_destroy_invalidates_the_object() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    check(create_hidden(window, recorder, preferences, error), "create for the external-destroy gate");
    HWND const hwnd = window.handle();
    window.apply(content_frame(2));

    // 外部（不是通过 WidgetWindow::destroy）把窗口销毁掉。
    ::DestroyWindow(hwnd);
    check(window.handle() == nullptr, "an external destroy invalidates the public handle");

    // 之后公开方法必须 no-op：不碰死句柄、不写偏好。
    int const saves_before = recorder.saves;
    window.apply(content_frame(3));
    window.show_from_request();
    check(recorder.saves == saves_before, "post-destroy calls write no preference");

    // destroy 必须幂等，而且第二次调用不能崩。
    window.destroy();
    window.destroy();
    check(window.handle() == nullptr, "destroy is idempotent");

    // 同一个对象还能重新建窗（类名相同，走 ERROR_CLASS_ALREADY_EXISTS 那一条）。
    Recorder again;
    check(create_hidden(window, again, preferences, error), "the object can be reused after destroy");
    check(window.handle() != nullptr, "the recreated window has a handle");
    window.destroy();
}

void test_capture_changed_ends_the_drag() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    preferences.anchor_known = true;
    preferences.anchor = POINT{1200, 900};
    check(create_hidden(window, recorder, preferences, error), "create for the capture gate");
    HWND const hwnd = window.handle();

    // 合成一次拖动：按下 → 移动超过阈值 → 捕获被外部抢走。
    // SetCapture 只在这两条 SendMessage 之间被持有，随后立刻同步释放，不移动光标。
    ::SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    ::SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(60, 70));
    int const saves_before = recorder.saves;
    ::ReleaseCapture();  // 触发 WM_CAPTURECHANGED

    check(recorder.saves == saves_before + 1,
          "losing the capture mid-drag still persists the moved anchor");
    check(recorder.last_saved.anchor_known, "the persisted anchor is the moved one");
    check(recorder.last_saved.anchor.x != 1200 || recorder.last_saved.anchor.y != 900,
          "the persisted anchor really moved");

    // 捕获已经没了：再移一次不该继续拖（否则就是在没按键的情况下把窗口拖走）。
    int const saves_after = recorder.saves;
    ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(200, 200));
    check(recorder.saves == saves_after, "after the capture is gone the window stops following");
    window.destroy();
}

void test_fullscreen_signal_names_and_shape() {
    using deskfeed::widget::FullscreenSignal;
    check(std::wstring(deskfeed::widget::fullscreen_signal_name(FullscreenSignal::kNone)) == L"none",
          "signal name none");
    check(std::wstring(deskfeed::widget::fullscreen_signal_name(
                           FullscreenSignal::kMonitorCovered)) == L"foreground-monitor-covered",
          "signal name monitor-covered");
    check(std::wstring(deskfeed::widget::fullscreen_signal_name(
                           FullscreenSignal::kD3DFullScreenExclusive)) == L"d3d-fullscreen-exclusive",
          "signal name d3d");

    // 只看“任何桌面状态下都成立”的不变量：返回值必须是三个之一，而且一定会留下一句说明。
    // 真正的全屏判定需要真实前台窗口，这里不宣称验证过。
    std::wstring detail;
    FullscreenSignal const signal = deskfeed::widget::fullscreen_foreground(&detail);
    check(signal == FullscreenSignal::kNone || signal == FullscreenSignal::kMonitorCovered ||
              signal == FullscreenSignal::kD3DFullScreenExclusive,
          "the fullscreen predicate returns a known signal");
    check(!detail.empty(), "the fullscreen predicate always explains itself");
}

void test_fonts_are_non_bold_and_metrics_are_tight() {
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput input;
    bool const gdiplus = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    check(gdiplus, "the test can start GDI+");

    HDC const dc = ::CreateCompatibleDC(nullptr);
    check(dc != nullptr, "the test has a measuring DC");
    deskfeed::widget::FontSet fonts;
    check(deskfeed::widget::create_fonts(dc, fonts), "create the widget fonts");
    check(!fonts.face.empty(), "a face was really resolved");

    // 整窗没有粗体：三个字体都必须是常规字重。
    HFONT const handles[] = {fonts.row_prefix, fonts.row_value, fonts.detail};
    for (HFONT font : handles) {
        LOGFONTW log{};
        check(::GetObjectW(font, sizeof(log), &log) == sizeof(log), "read back a font's LOGFONT");
        check(log.lfWeight == FW_NORMAL, "the widget has no bold font");
    }

    // 紧测量：量出来的宽度就是 GDI 的裸宽度，没有再减一层留白。
    HGDIOBJ const previous = ::SelectObject(dc, fonts.row_value);
    SIZE reference{};
    ::GetTextExtentPoint32W(dc, L"MM", 2, &reference);
    ::SelectObject(dc, previous);
    check(deskfeed::widget::tight_width(dc, L"MM", fonts.row_value) == reference.cx,
          "tight_width is exactly the GDI extent (no extra padding)");

    deskfeed::widget::TextMetrics metrics{};
    deskfeed::widget::compute_text_metrics(dc, fonts, metrics, gdiplus);
    check(metrics.row_height > 0, "row height is measured");
    check(metrics.space_width >= 1, "the space width is measured");
    if (gdiplus) {
        int const dpi = ::GetDeviceCaps(dc, LOGPIXELSY);
        int const gdiplus_height =
            deskfeed::widget::gdiplus_line_height(fonts.face, fonts.row_points, dpi, false);
        if (gdiplus_height > 0) {
            check(metrics.row_height == gdiplus_height,
                  "with GDI+ available the row height uses the GDI+ line height");
        }
    }

    // GDI+ 不可用时（N1）：一个 GDI+ 调用都不做，仍然给出 GDI 兜底的行高。
    deskfeed::widget::TextMetrics fallback{};
    deskfeed::widget::compute_text_metrics(dc, fonts, fallback, false);
    check(fallback.row_height > 0, "without GDI+ the row height falls back to the GDI metric");
    check(fallback.space_width >= 1, "the space width still works without GDI+");

    deskfeed::widget::destroy_fonts(fonts);
    ::DeleteDC(dc);
    if (gdiplus) {
        Gdiplus::GdiplusShutdown(token);
    }
}

// 整体失效（alarm）不是“没有值”：两条目仍然排两行，不能塌成一行 ERROR 的高度。
void test_alarm_keeps_two_rows_height() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    check(create_hidden(window, recorder, preferences, error), "create for the alarm-height gate");
    HWND const hwnd = window.handle();

    window.apply(content_frame(2));
    SIZE const normal = window_size(hwnd);

    deskfeed::Frame alarmed = content_frame(2);
    alarmed.alarm = true;
    alarmed.reason = "transport failed";
    window.apply(alarmed);
    SIZE const with_alarm = window_size(hwnd);
    check(with_alarm.cy == normal.cy, "an alarm frame keeps the same two-row height");

    deskfeed::Frame failed;
    failed.kind = deskfeed::ViewKind::Error;
    failed.reason = "no url";
    window.apply(failed);
    SIZE const with_error = window_size(hwnd);
    check(with_error.cy < normal.cy,
          "an ERROR frame is shorter (one row), so the alarm case really differs");
    window.destroy();
}

// 隐藏窗口上的 WM_PAINT 冒烟：能画、不崩、不显示。
void test_hidden_paint_smoke() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    check(create_hidden(window, recorder, preferences, error), "create for the paint smoke test");
    HWND const hwnd = window.handle();
    window.apply(content_frame(3));
    ::SendMessageW(hwnd, WM_PAINT, 0, 0);
    check(::IsWindow(hwnd), "the window survives a WM_PAINT while hidden");
    check(!::IsWindowVisible(hwnd), "the paint smoke test never shows the window");
    window.destroy();
}

// 拖动收尾时落盘失败：锚点必须回滚，之后成功落盘写回的是**旧**锚点。
void test_drag_save_failure_rolls_the_anchor_back() {
    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    std::wstring error;
    deskfeed::widget::WindowPreferences preferences;
    preferences.anchor_known = true;
    preferences.anchor = POINT{800, 600};
    check(create_hidden(window, recorder, preferences, error), "create for the drag rollback gate");
    HWND const hwnd = window.handle();

    // 合成一次拖动，并在捕获被抢走之前把落盘通道打成失败。
    // SetCapture 只在这两条 SendMessage 之间被持有，随后立刻同步释放，不移动光标。
    ::SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    ::SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(80, 90));
    recorder.save_ok = false;
    int const saves_before = recorder.saves;
    ::ReleaseCapture();  // 触发 WM_CAPTURECHANGED → 拖动收尾 → 落盘失败
    check(recorder.saves == saves_before + 1, "the drag finish attempted to persist the anchor");
    check(recorder.last_saved.anchor.x != 800 || recorder.last_saved.anchor.y != 600,
          "the attempted value is the moved anchor");
    recorder.save_ok = true;

    // 下一次成功的落盘必须写回**旧**锚点：失败那一次已经把内存里的锚点回滚了。
    ::SendMessageW(hwnd, WM_COMMAND, kCmdTopmost, 0);
    check(recorder.last_saved.anchor_known && recorder.last_saved.anchor.x == 800 &&
              recorder.last_saved.anchor.y == 600,
          "after a failed drag save the anchor is rolled back to the old corner");
    window.destroy();
}

// ---- 菜单保活：置顶守护不得把宿主窗口抬到它自己那个菜单之上 ------------------------------------
//
// 这一条走**真的** `TrackPopupMenu` 模态循环（不是按命令号发 `WM_COMMAND`）：坏掉的是“菜单还开着
// 的时候窗口被重新置顶”这一件事，它只在真菜单的模态循环里成立——菜单是本窗口**拥有**的顶层弹出
// 窗口，`SetWindowPos(hwnd, HWND_TOPMOST, ...)` 会把宿主抬到自己菜单之上（实测：补一次置顶后，
// 菜单矩形内 `WindowFromPoint` 命中的就从菜单变成宿主窗口）。菜单被自家窗口盖住、却仍然握着鼠标
// 捕获，于是随后的点击会落进这个**看不见的**菜单、选中光标下那一项——这**可以**产生与现场相同的
// 隐藏/退出后果，时序（菜单弹出后约一个 tick 就补置顶、再约半秒后 hide/quit）也吻合；现场那一次
// 的退出**来源尚未捕获**（菜单命令与外部 `WM_CLOSE` 在旧日志里分不出来，运行期日志现在才补上）。
// 这不是内存/异常崩溃，是“菜单被遮盖但仍捕获输入 → 后续误触发 Hide/Exit 路径”。
//
// 不碰用户桌面：整轮跑在**私有桌面**上（`CreateDesktop`，不是输入桌面），用户屏幕、光标与焦点都
// 不受影响，也不注入任何真实输入——只给自己的窗口发合成消息；菜单与窗口都是本进程自己的窗口。
//
// 触发用守卫两条具名判据里的 `is_topmost_of()==false`（“置顶位丢了”），测试期间把宿主窗口的
// topmost 位剥掉来定死它。生产日志里那次命中的是另一条分支（z 序扫描认为有异进程窗口压着我们；
// `covered_by_foreign_window_of` 会跳过本进程的菜单，而离线环境造不出异进程窗口）。两条判据落到
// 的是**同一行动作** `apply_topmost_of`，也就是这条门禁钉住的东西；差别只在这里写明。
constexpr wchar_t kMenuGateClass[] = L"DeskFeedWidgetMenuGateWindow";
constexpr UINT_PTR kMenuGateTimer = 11;
constexpr unsigned kMenuGateTickMs = 100;

struct MenuGateState {
    HWND widget = nullptr;
    HWND helper = nullptr;
    HWND menu = nullptr;
    unsigned long long start = 0;
    int phase = 0;
    bool harness_ok = false;           // 菜单真的开出来了，而且它与窗口有交叠区域
    bool visible_before = false;       // 守卫动手之前：交叠点上最上层是菜单（用户看得见）
    bool visible_after_guard = false;  // 守卫动手之后：仍然是菜单 ← 这一条就是红/绿判据
    bool topmost_restored = false;     // 菜单结束后：守卫按原判据把置顶补了回来
    bool end_menu_called = false;
    bool widget_created = false;
    bool repeated_request_sent = false;  // 菜单开着的时候又递了一条右键请求
    int quit_calls = 0;
    int hidden_saves = 0;
};

MenuGateState* g_menu_gate = nullptr;

bool create_for_menu_gate(deskfeed::widget::WidgetWindow& window, Recorder& recorder,
                          deskfeed::widget::WindowPreferences preferences, std::wstring& error) {
    deskfeed::widget::WidgetOptions options;
    options.instance = ::GetModuleHandleW(nullptr);
    options.class_name = kMenuGateClass;  // 独立类名：与其它用例的隐藏窗口互不干扰
    options.taskbar_available = true;
    options.force_hidden = false;  // 这一轮要真的显示（显示在私有桌面上）
    // 私有桌面上没有真实的 Shell 通知区域；图标开关关掉，菜单本身仍然由窗口自己建。
    options.tray_icon_available = false;
    options.preferences = preferences;
    return window.create(options, callbacks_for(recorder), &error);
}

// 弹出菜单的窗口类固定是 `#32768`；它属于开菜单的那个线程。
HWND find_popup_menu_of_this_thread() {
    struct Search {
        HWND found;
    } search{nullptr};
    ::EnumThreadWindows(
        ::GetCurrentThreadId(),
        [](HWND window, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<Search*>(param);
            wchar_t name[64] = L"";
            ::GetClassNameW(window, name, static_cast<int>(std::size(name)));
            if (::wcscmp(name, L"#32768") == 0) {
                state->found = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// 菜单与宿主窗口的交叠点：只有两边都盖住它，“谁在最上层”才有意义。
bool menu_overlap_point(HWND widget, HWND menu, POINT* out) {
    RECT mine{}, menu_rect{}, overlap{};
    if (widget == nullptr || menu == nullptr || out == nullptr || !::GetWindowRect(widget, &mine) ||
        !::GetWindowRect(menu, &menu_rect) || !::IntersectRect(&overlap, &mine, &menu_rect)) {
        return false;
    }
    *out = POINT{(overlap.left + overlap.right) / 2, (overlap.top + overlap.bottom) / 2};
    return true;
}

LRESULT CALLBACK menu_gate_helper_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    MenuGateState* const state = g_menu_gate;
    if (message != WM_TIMER || wparam != kMenuGateTimer || state == nullptr) {
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }
    unsigned long long const elapsed = ::GetTickCount64() - state->start;
    if (state->phase == 0 && elapsed >= 250) {
        state->menu = find_popup_menu_of_this_thread();
        POINT at{0, 0};
        state->harness_ok = state->menu != nullptr && menu_overlap_point(state->widget, state->menu,
                                                                        &at);
        state->visible_before = state->harness_ok && ::WindowFromPoint(at) == state->menu;
        // “置顶位丢了”——守卫存在的理由，也是这次定死的触发（见上面那段说明）。
        ::SetWindowPos(state->widget, HWND_NOTOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        state->phase = 1;
        return 0;
    }
    if (state->phase == 1 && elapsed >= 2000) {
        // 守卫定时器是 1000 ms 一次：剥掉置顶位之后至少跨过一整轮，它必然在菜单开着的时候跑过。
        POINT at{0, 0};
        state->visible_after_guard = menu_overlap_point(state->widget, state->menu, &at) &&
                                     ::WindowFromPoint(at) == state->menu;
        ::EndMenu();  // 自己线程上那个活动菜单：取消，不该选中任何一项
        state->end_menu_called = true;
        state->phase = 2;
        return 0;
    }
    if (state->phase == 1 && elapsed >= 700 && !state->repeated_request_sent) {
        // 菜单开着的时候再递一条右键请求：真实手势里菜单握着捕获、窗口收不到它，但外部投递
        // 得到（离线夹具就是这么发的）。它必须被挡在门外——既不弹第二个菜单，也不能把
        // menu_open 清掉让守卫提前恢复。
        state->repeated_request_sent = true;
        ::PostMessageW(state->widget, WM_RBUTTONUP, 0, MAKELPARAM(5, 5));
        ::PostMessageW(state->widget, WM_CONTEXTMENU, 0, MAKELPARAM(5, 5));
        return 0;
    }
    if (state->phase == 2 && is_topmost(state->widget)) {
        // 守卫的下一次 tick 把置顶补回来（只是延后，不是禁用）。等它真的补上就收工，
        // 不再死等一段固定时间。
        state->topmost_restored = true;
        ::KillTimer(hwnd, kMenuGateTimer);
        state->phase = 3;
        return 0;
    }
    if (state->phase == 2 && elapsed >= 6000) {
        // 兜底：守卫始终没有补回来 ⇒ 交给断言报红，别把用例挂死。
        ::KillTimer(hwnd, kMenuGateTimer);
        state->phase = 3;
        return 0;
    }
    return 0;
}

DWORD WINAPI menu_gate_thread(void* param) {
    auto* const state = static_cast<MenuGateState*>(param);
    HDESK const original = ::GetThreadDesktop(::GetCurrentThreadId());
    HDESK const desktop = ::CreateDesktopW(L"DeskFeedWidgetMenuGateDesk", nullptr, nullptr, 0,
                                           GENERIC_ALL, nullptr);
    if (desktop == nullptr || !::SetThreadDesktop(desktop)) {
        if (desktop != nullptr) {
            ::CloseDesktop(desktop);
        }
        return 1;
    }

    WNDCLASSEXW helper_class{};
    helper_class.cbSize = sizeof(helper_class);
    helper_class.lpfnWndProc = &menu_gate_helper_proc;
    helper_class.hInstance = ::GetModuleHandleW(nullptr);
    helper_class.lpszClassName = L"DeskFeedWidgetMenuGateHelper";
    ::RegisterClassExW(&helper_class);

    Recorder recorder;
    deskfeed::widget::WidgetWindow window;
    POINT cursor{};
    ::GetCursorPos(&cursor);  // 私有桌面上的光标位置：菜单就弹在这里
    deskfeed::widget::WindowPreferences preferences;
    preferences.anchor_known = true;
    preferences.anchor = POINT{cursor.x + 200, cursor.y + 100};
    preferences.topmost = true;
    preferences.hidden = false;
    std::wstring error;
    state->widget_created = create_for_menu_gate(window, recorder, preferences, error);
    if (state->widget_created) {
        window.apply(content_frame(2));
        state->widget = window.handle();
        // 菜单来源用**通知区域图标**的右键回调：它必须打开悬浮窗那一份菜单（同一个 HMENU）。
        // 这里仍把窗口摆到菜单要出现的位置，验证的是菜单与置顶守护的关系，与手势来源无关。
        RECT rect{};
        if (state->widget != nullptr && ::GetWindowRect(state->widget, &rect)) {
            ::SetWindowPos(state->widget, nullptr, cursor.x, cursor.y, rect.right - rect.left,
                           rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        state->helper = ::CreateWindowExW(0, L"DeskFeedWidgetMenuGateHelper", L"h", WS_POPUP, 0, 0,
                                          0, 0, nullptr, nullptr, helper_class.hInstance, nullptr);
        if (state->widget != nullptr && state->helper != nullptr) {
            state->start = ::GetTickCount64();
            ::SetTimer(state->helper, kMenuGateTimer, kMenuGateTickMs, nullptr);
            ::PostMessageW(state->widget, deskfeed::widget::kTrayIconMessage, 0,
                           MAKELPARAM(WM_CONTEXTMENU, 1));
            unsigned long long const deadline = state->start + 9000;
            MSG message{};
            while (::GetTickCount64() < deadline && state->phase < 3) {
                while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    ::TranslateMessage(&message);
                    ::DispatchMessageW(&message);
                }
                ::Sleep(5);
            }
        }
    }
    window.destroy();  // 偏好最后一次落盘也走这里；回调上下文必须活到它返回之后
    state->quit_calls = recorder.quit;
    state->hidden_saves = recorder.hidden_saves;
    if (state->helper != nullptr) {
        ::DestroyWindow(state->helper);
        state->helper = nullptr;
    }
    state->widget = nullptr;
    ::SetThreadDesktop(original);
    ::CloseDesktop(desktop);
    return 0;
}

std::wstring menu_gate_log_path() {
    wchar_t base[MAX_PATH] = L"";
    DWORD const length = ::GetTempPathW(MAX_PATH, base);
    std::wstring directory(base, length);
    directory += L"deskfeed-widget-menu-gate-" + std::to_wstring(::GetCurrentProcessId());
    ::CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\widget-menu-gate.log";
}

std::string read_file_bytes(std::wstring const& path) {
    HANDLE const file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    LARGE_INTEGER size{};
    std::string bytes;
    if (::GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        if (!::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) {
            bytes.clear();
        } else {
            bytes.resize(read);
        }
    }
    ::CloseHandle(file);
    return bytes;
}

void test_context_menu_stays_above_the_widget_through_the_topmost_guard() {
    std::wstring const log_path = menu_gate_log_path();
    // 这一条同时用**生产日志**取证：z 序只能说明“菜单被盖住”，说明不了“守卫在菜单期间动过手”。
    bool const logged = deskfeed::log_open(log_path, false);
    check(logged, "the menu gate captures the widget's own production log");

    MenuGateState state;
    g_menu_gate = &state;
    HANDLE const thread = ::CreateThread(nullptr, 0, &menu_gate_thread, &state, 0, nullptr);
    bool joined = false;
    if (thread != nullptr) {
        // 只有真的等到线程退出，才关句柄、才读 state：超时说明夹具自己挂住了（那时 state 还在被
        // 那个线程写），继续读就是读一块已经回收的栈。这里不强杀任何进程，只结束自己的测试进程。
        joined = ::WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0;
        if (joined) {
            ::CloseHandle(thread);
        }
    }
    g_menu_gate = nullptr;
    check(thread != nullptr, "the menu gate desktop thread started");
    check(joined, "the menu gate thread exited in time (menu cancelled and joined)");
    if (!joined) {
        // fail-fast：不读 state、不假装线程已经退出，直接让测试进程以失败码结束（线程随进程结束）。
        std::printf("FAIL the menu gate thread did not exit; aborting before reading its stack\n");
        std::fflush(stdout);
        ::ExitProcess(1);
    }

    check(state.widget_created, "the menu gate created the production widget window");
    check(state.harness_ok, "the real context menu opened on the private desktop, over the widget");
    check(state.visible_before, "before the guard acts the menu is the top window over its own area");
    check(state.end_menu_called, "the gate cancelled the menu instead of leaving it up");
    check(state.visible_after_guard,
          "the topmost guard does not raise the widget over its own open menu");
    check(state.topmost_restored,
          "after the menu closes the guard restores topmost exactly as before");
    check(state.repeated_request_sent, "the gate really sent a repeated right-click request");
    check(state.quit_calls == 0, "cancelling the menu (and the repeated request) never asks to quit");
    check(state.hidden_saves == 0, "cancelling the menu (and the repeated request) never hides");

    deskfeed::log_close();
    // 全部判据都是 ASCII 子串（日志行首只有时间戳与线程号），不需要解码 UTF-8。
    std::string const text = read_file_bytes(log_path);
    std::size_t const opened = text.rfind("widget: context menu texts:");
    std::size_t const closed =
        opened == std::string::npos
            ? std::string::npos
            : text.find("widget: context menu closed:", opened);
    check(opened != std::string::npos && closed != std::string::npos,
          "the production log recorded both the menu opening and its return command");
    if (opened != std::string::npos && closed != std::string::npos) {
        check(text.substr(opened, closed - opened).find("the topmost bit was lost") ==
                  std::string::npos,
              "the topmost guard never re-applied while our own menu was open");
        check(text.find("the topmost bit was lost", closed) != std::string::npos,
              "the guard re-applied right after the menu closed (deferred, not disabled)");
        // 菜单开着时那条重复右键请求必须被挡在门外：整轮只能有一次“弹出菜单”。
        std::size_t hits = 0;
        for (std::size_t at = text.find("widget: context menu texts:"); at != std::string::npos;
             at = text.find("widget: context menu texts:", at + 1)) {
            ++hits;
        }
        check(hits == 1, "a repeated right-click request while the menu is open opens no second menu");
        check(text.find("widget: context menu closed: command=0") != std::string::npos,
              "the production log records the menu returning the cancel command");
    }
    ::DeleteFileW(log_path.c_str());
    std::wstring directory = log_path;
    std::wstring::size_type const slash = directory.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        ::RemoveDirectoryW(directory.substr(0, slash).c_str());
    }
}

}  // namespace

int main() {
    test_context_menu_stays_above_the_widget_through_the_topmost_guard();
    test_create_is_hidden_and_tooltips_are_configured();
    test_menu_toggles_callbacks_rollback_and_refresh();
    test_show_request_leaves_disk_alone_while_force_hidden();
    test_tray_single_click_is_inert_and_double_click_requests_show();
    test_anchor_is_not_rewritten_by_content_churn();
    test_external_destroy_invalidates_the_object();
    test_capture_changed_ends_the_drag();
    test_alarm_keeps_two_rows_height();
    test_hidden_paint_smoke();
    test_drag_save_failure_rolls_the_anchor_back();
    test_hover_shows_each_nodes_own_tip();
    test_hover_in_detailed_mode_covers_only_nodes();
    test_hover_same_node_survives_refresh();
    test_double_click_toggles_and_persists();
    test_drag_does_not_toggle_the_mode();
    test_hover_and_detail_stress_keeps_state_consistent();
    test_fullscreen_signal_names_and_shape();
    test_fonts_are_non_bold_and_metrics_are_tight();

    if (g_failures == 0) {
        std::printf("widget window gate: all passed\n");
        return 0;
    }
    std::printf("widget window gate: %d failure(s)\n", g_failures);
    return 1;
}
