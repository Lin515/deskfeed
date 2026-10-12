// 悬浮窗的**隐藏窗口门禁**：在测试进程自己的线程上建一个真窗口，但用 `force_hidden = true`
// 让它永远不显示，所以真实悬浮窗绝不会出现在桌面上。
//
// 这里只发**合成窗口消息**与直接调用公开方法：不移动光标、不弹可见菜单、不做真实悬停、
// 不注入 explorer、不碰用户正在跑的实例。需要真实指针或真实桌面的行为（悬停提示真的弹出、
// 命中测试、拖动落盘后的真机位置、全屏让位）不在这里覆盖，也不宣称验证过。
//
// `WM_COMMAND` 的命令号是窗口对外的稳定接口（菜单项与离线夹具共用），这里按号发送。

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <objidl.h>
#include <gdiplus.h>

#include <cstdio>
#include <string>

#include "display/frame.h"
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
    options.preferences = preferences;
    return window.create(options, callbacks_for(recorder), &error);
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
        check(::SendMessageW(tip, TTM_GETTOOLCOUNT, 0, 0) == 3, "one hover tool per visible line");
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

}  // namespace

int main() {
    test_create_is_hidden_and_tooltips_are_configured();
    test_menu_toggles_callbacks_rollback_and_refresh();
    test_show_request_leaves_disk_alone_while_force_hidden();
    test_anchor_is_not_rewritten_by_content_churn();
    test_external_destroy_invalidates_the_object();
    test_capture_changed_ends_the_drag();
    test_fullscreen_signal_names_and_shape();
    test_fonts_are_non_bold_and_metrics_are_tight();

    if (g_failures == 0) {
        std::printf("widget window gate: all passed\n");
        return 0;
    }
    std::printf("widget window gate: %d failure(s)\n", g_failures);
    return 1;
}
