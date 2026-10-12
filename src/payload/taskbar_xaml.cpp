#include "payload/taskbar_xaml.h"

#include "core/utf8.h"
#include "payload/action_worker.h"
#include "payload/hook_post.h"
#include "payload/reg_hook.h"
#include "payload/xaml_tap.h"
#include "platform/log.h"
#include "platform/win32_util.h"

#include <windows.h>

#include <commctrl.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <atomic>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace deskfeed::payload {
namespace {

namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxd = winrt::Windows::UI::Xaml::Documents;
namespace wuxi = winrt::Windows::UI::Xaml::Input;
namespace wuxm = winrt::Windows::UI::Xaml::Media;

// 任务栏托盘面板里那个容器的名字。读数是插进它的布局，不是叠在它上面。
constexpr wchar_t kTrayElementName[] = L"SystemTrayFrameGrid";
constexpr wchar_t kBlockName[] = L"DeskFeedBlock";
constexpr wchar_t kReadingName[] = L"DeskFeedReading";

// 字号：从 12 往下试，地板 8；托盘给的高度放不下就每行多放一个条目（行数因此变少）再来一遍。
constexpr double kFontSize = 12.0;
constexpr double kFontFloor = 8.0;
constexpr double kFontStep = 1.0;
// 离树测量的高度要多留一点余量才算放得下（实测"离树测量"与真元素 arrange 出来的高度逐像素一致）。
constexpr double kHeightSlack = 1.0;
// 量不到托盘高度时的兜底：Windows 11 任务栏那一档。
constexpr double kFallbackTrayHeight = 48.0;
// 块的宽度上限：屏幕宽度的 45%，与旧挂件贴靠条同一个量级；超出画省略号。
constexpr double kMaxWidthRatio = 0.45;

// 走查要等核心把站点交出来；连接诊断之后 XAML 还需要一点时间把每个核心接上。
constexpr int kSiteAttempts = 10;
constexpr unsigned long kSiteDelayMs = 300;
// 投递给任务栏线程的等待上限。超时**不是**失败，是"这次没拿到静止证明"。
constexpr unsigned kPostTimeoutMs = 8000;

constexpr int kHoverInitialDelayMs = 250;
constexpr int kHoverMaxTipWidth = 420;

struct HoverCell {
    std::wstring tip;
    double left = 0.0;
    double right = 0.0;
    double top = 0.0;
    double bottom = 0.0;

    bool hit(double x, double y) const {
        return x >= left && x < right && y >= top && y < bottom;
    }
};

struct Segment {
    std::wstring text;
    winrt::Windows::UI::Color color{};
    bool line_break = false;
};

struct State {
    HWND tray = nullptr;
    // 自测宿主给的宿主窗口（没有 Shell_TrayWnd 时用它当悬停提示的属主与钩子收尾的确认窗口）。
    HWND host_window = nullptr;
    size_t site_index = 0;

    // 插进去的 XAML 对象。只在任务栏线程上被碰，放在这里是为了活过插入调用。
    wuxc::Border block{nullptr};
    wuxc::TextBlock label{nullptr};
    wuxc::Grid column_owner{nullptr};
    wuxc::ColumnDefinition column{nullptr};
    std::vector<wux::FrameworkElement> shifted;
    std::vector<wuxd::Run> runs;
    std::vector<Segment> applied;

    // 解析出来的托盘面板，**长期持有**：句柄只来自那唯一一次走查，解析一次之后诊断连接就关掉了，
    // 所以它是运行期唯一能碰的任务栏 XAML 入口（补插、验证都用它）。只在任务栏线程上被碰。
    wux::FrameworkElement tray_element{nullptr};

    // 悬停命中模型：每个条目在块内的矩形与它的 tip，以及"块内但不属于任何条目"时的整份详情。
    std::vector<HoverCell> hover_cells;
    std::wstring hover_detail;
    int hover_last_hit = -2;
    double hover_width = 0.0;
    double hover_height = 0.0;
    double hover_font = 0.0;
    POINT hover_origin{};
    bool hover_origin_known = false;

    // 自测宿主专用。
    unsigned slow_apply_ms = 0;
    bool probe_placeholder = false;
    bool probe_inline = false;
};

State g_state;

// 显示内容的进程内拷贝：负载线程写、任务栏线程读。
std::mutex g_content_mutex;
deskfeed::ReadingContent g_content;
bool g_have_content = false;

std::atomic<bool> g_block_present{false};
std::atomic<bool> g_tray_element_held{false};
std::atomic<bool> g_diagnostics_closed{false};
std::atomic<bool> g_attached{false};
std::atomic<bool> g_probe_ack_window_used{false};

HWND g_tooltip = nullptr;
HWND g_ack_window_override = nullptr;

std::wstring to_wide(std::string const& text) { return deskfeed::wide_from_utf8(text); }

// 深浅色与取色：XAML 自己的判定优先（ActualTheme 天然跟随系统深浅色切换），拿不到再退回系统设置。
bool light_taskbar(wux::FrameworkElement const& element) {
    if (element != nullptr) {
        try {
            wux::ElementTheme const theme = element.ActualTheme();
            if (theme == wux::ElementTheme::Light) {
                return true;
            }
            if (theme == wux::ElementTheme::Dark) {
                return false;
            }
        } catch (winrt::hresult_error const&) {
            // 取不到就当没表态，走下面的兜底。
        }
    }

    DWORD light = 0;
    DWORD size = sizeof(light);
    DWORD type = 0;
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                        KEY_READ, &key) == ERROR_SUCCESS) {
        if (::RegQueryValueExW(key, L"SystemUsesLightTheme", nullptr, &type,
                               reinterpret_cast<LPBYTE>(&light), &size) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            light = light != 0 ? 1 : 0;
        }
        ::RegCloseKey(key);
    }
    return light != 0;
}

struct Colors {
    winrt::Windows::UI::Color value{};
    winrt::Windows::UI::Color prefix{};
    winrt::Windows::UI::Color warn{};
    winrt::Windows::UI::Color error{};
    bool light = false;
};

Colors colors_for(bool light) {
    Colors colors;
    colors.light = light;
    if (light) {
        // 浅色任务栏上白字等于看不见：深字。
        colors.value = winrt::Windows::UI::ColorHelper::FromArgb(255, 24, 24, 28);
        colors.prefix = winrt::Windows::UI::ColorHelper::FromArgb(255, 92, 92, 100);
    } else {
        colors.value = winrt::Windows::UI::ColorHelper::FromArgb(255, 242, 242, 244);
        colors.prefix = winrt::Windows::UI::ColorHelper::FromArgb(255, 139, 139, 147);
    }
    colors.warn = winrt::Windows::UI::ColorHelper::FromArgb(255, 232, 165, 66);
    colors.error = winrt::Windows::UI::ColorHelper::FromArgb(255, 213, 73, 65);
    return colors;
}

winrt::Windows::UI::Color tone_color(ReadingTone tone, Colors const& colors) {
    switch (tone) {
        case ReadingTone::Warn:
            return colors.warn;
        case ReadingTone::Error:
            return colors.error;
        case ReadingTone::Normal:
            break;
    }
    return colors.value;
}

UINT window_dpi(HWND window) {
    using GetDpiForWindowType = UINT(WINAPI*)(HWND);
    static auto get_dpi_for_window = reinterpret_cast<GetDpiForWindowType>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (get_dpi_for_window != nullptr && window != nullptr) {
        UINT const value = get_dpi_for_window(window);
        if (value != 0) {
            return value;
        }
    }
    return 96;
}

double block_max_width() {
    int const screen_width = ::GetSystemMetrics(SM_CXSCREEN);
    HWND const window = g_state.tray != nullptr ? g_state.tray : ::GetDesktopWindow();
    return kMaxWidthRatio * screen_width * 96.0 / static_cast<double>(window_dpi(window));
}

// 读数的一"段"：文字 + 取色。前缀弱色、数值按条目的 tone 取色，条目之间是 " · "；
// line_break 段是换行（落到 TextBlock 里是一个 LineBreak）。换行也必须进段模型：刷新时
// "段数与段序都没变"是复用已有 Inline 的唯一判据。
std::vector<Segment> reading_segments(deskfeed::ReadingContent const& content, Colors const& colors,
                                      size_t per_line) {
    std::vector<Segment> segments;
    std::vector<deskfeed::ReadingLine> const lines = deskfeed::reading_pack(content, per_line);
    for (size_t line_index = 0; line_index < lines.size(); ++line_index) {
        if (line_index != 0) {
            segments.push_back(Segment{std::wstring(), colors.prefix, true});
        }
        deskfeed::ReadingLine const& line = lines[line_index];
        for (size_t index = 0; index < line.segments.size(); ++index) {
            if (index != 0) {
                segments.push_back(Segment{L" · ", colors.prefix, false});
            }
            deskfeed::ReadingRow const& row = line.segments[index];
            if (!row.label.empty()) {
                segments.push_back(Segment{to_wide(row.label) + L" ", colors.prefix, false});
            }
            segments.push_back(Segment{to_wide(row.value), tone_color(row.tone, colors), false});
        }
    }
    return segments;
}

void apply_segments(wuxc::TextBlock const& label, std::vector<Segment> const& segments) {
    label.Inlines().Clear();
    g_state.runs.clear();
    for (Segment const& segment : segments) {
        if (segment.line_break) {
            label.Inlines().Append(wuxd::LineBreak());
            continue;
        }
        wuxd::Run run;
        run.Text(segment.text);
        run.Foreground(wuxm::SolidColorBrush(segment.color));
        label.Inlines().Append(run);
        g_state.runs.push_back(run);
    }
    g_state.applied = segments;
}

bool same_segments(std::vector<Segment> const& left, std::vector<Segment> const& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (left[index].line_break != right[index].line_break || left[index].text != right[index].text) {
            return false;
        }
    }
    return true;
}

// 离树测量：把同一份段模型放进一个临时 TextBlock 量一次。托盘给的高度放不放得下就靠它判断。
winrt::Windows::Foundation::Size measure_segments(std::vector<Segment> const& segments, double font,
                                                  double max_width) {
    wuxc::TextBlock probe;
    probe.FontSize(font);
    probe.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    probe.MaxWidth(max_width);
    for (Segment const& segment : segments) {
        if (segment.line_break) {
            probe.Inlines().Append(wuxd::LineBreak());
            continue;
        }
        wuxd::Run run;
        run.Text(segment.text);
        probe.Inlines().Append(run);
    }
    probe.Measure(winrt::Windows::Foundation::Size{
        static_cast<float>(max_width), std::numeric_limits<float>::infinity()});
    return probe.DesiredSize();
}

// 托盘元素能给我们多少高度（DIP）。量不到就退回 Shell_TrayWnd 的客户区高度，再退回 48。
double available_height(wux::FrameworkElement const& tray_element) {
    try {
        if (tray_element != nullptr) {
            double const height = tray_element.ActualHeight();
            if (height > 0.0) {
                return height;
            }
        }
    } catch (winrt::hresult_error const&) {
    }
    if (g_state.tray != nullptr) {
        RECT client{};
        if (::GetClientRect(g_state.tray, &client) != FALSE && client.bottom > 0) {
            double const scale = static_cast<double>(window_dpi(g_state.tray)) / 96.0;
            return client.bottom / (scale > 0.0 ? scale : 1.0);
        }
    }
    return kFallbackTrayHeight;
}

struct ReadingPlan {
    size_t per_line = 1;
    double font = kFontSize;
    size_t lines = 1;
    double width = 0.0;
    double height = 0.0;
    bool fits = false;
};

// 折行与字号：先按条目数算自动折行（最多 3 行），再按托盘给的高度从 12 往下试字号；一路试到
// 地板都放不下时不硬塞，改成每行多放一个条目（行数因此变少）再来一遍。
ReadingPlan plan_reading(deskfeed::ReadingContent const& content, Colors const& colors,
                         double max_width, double available) {
    ReadingPlan plan;
    plan.per_line = deskfeed::reading_per_line_for(content.rows.size());
    if (plan.per_line == 0) {
        plan.per_line = 1;
    }

    std::vector<std::wstring> candidate_note;
    for (size_t per_line = plan.per_line; per_line <= deskfeed::kMaxSegmentsPerLine; ++per_line) {
        std::vector<Segment> const segments = reading_segments(content, colors, per_line);
        double last_height = 0.0;
        for (double font = kFontSize; font >= kFontFloor; font -= kFontStep) {
            auto const size = measure_segments(segments, font, max_width);
            last_height = size.Height;
            if (static_cast<double>(size.Height) + kHeightSlack <= available) {
                plan.per_line = per_line;
                plan.font = font;
                plan.lines = deskfeed::reading_pack(content, per_line).size();
                plan.width = size.Width;
                plan.height = size.Height;
                plan.fits = true;
                log_format(L"reading plan: rows=%zu per_line=%zu lines=%zu measured=%.1fx%.1f "
                           L"available=%.1f font=%.1f fits=yes",
                           content.rows.size(), plan.per_line, plan.lines,
                           static_cast<double>(size.Width), static_cast<double>(size.Height),
                           available, plan.font);
                return plan;
            }
            candidate_note.push_back(std::to_wstring(static_cast<int>(font)) + L"pt=" +
                                     std::to_wstring(static_cast<int>(size.Height)));
        }
        log_format(L"reading plan: per_line=%zu does not fit (%.1f > %.1f); wrapping further",
                   per_line, last_height, available);
    }

    plan.per_line = deskfeed::kMaxSegmentsPerLine;
    plan.font = kFontFloor;
    std::vector<Segment> const segments = reading_segments(content, colors, plan.per_line);
    auto const size = measure_segments(segments, plan.font, max_width);
    plan.lines = deskfeed::reading_pack(content, plan.per_line).size();
    plan.width = size.Width;
    plan.height = size.Height;
    log_line(L"reading plan: nothing fits even at the font floor; using the widest wrapping");
    (void)candidate_note;
    return plan;
}

// 每个段落的屏幕宽度：悬停命中要用它把"指针在块内的 DIP 位置"落到具体条目上。
double segment_width(std::wstring const& text, double font) {
    if (text.empty()) {
        return 0.0;
    }
    wuxc::TextBlock probe;
    probe.FontSize(font);
    probe.Text(text);
    probe.Measure(winrt::Windows::Foundation::Size{std::numeric_limits<float>::infinity(),
                                                  std::numeric_limits<float>::infinity()});
    return probe.DesiredSize().Width;
}

void rebuild_hover_model(deskfeed::ReadingContent const& content, ReadingPlan const& plan,
                         double width, double height) {
    g_state.hover_cells.clear();
    g_state.hover_width = width;
    g_state.hover_height = height;
    g_state.hover_font = plan.font;
    g_state.hover_last_hit = -2;

    std::vector<deskfeed::ReadingLine> const lines = deskfeed::reading_pack(content, plan.per_line);
    std::wstring detail;
    for (deskfeed::ReadingRow const& row : content.rows) {
        if (!row.detail.empty()) {
            if (!detail.empty()) {
                detail += L"\n";
            }
            detail += to_wide(row.label.empty() ? row.value : row.label + " " + row.value);
            detail += L"\n  " + to_wide(row.detail);
        }
    }
    g_state.hover_detail = detail;

    double top = 0.0;
    for (deskfeed::ReadingLine const& line : lines) {
        double const line_height = segment_width(L"Xg", plan.font);
        double left = 0.0;
        for (size_t index = 0; index < line.segments.size(); ++index) {
            deskfeed::ReadingRow const& row = line.segments[index];
            std::wstring prefix = row.label.empty() ? std::wstring() : to_wide(row.label) + L" ";
            double const row_width =
                segment_width(prefix + to_wide(row.value), plan.font);
            HoverCell cell;
            cell.tip = row.tip.empty() ? deskfeed::wide_from_utf8(deskfeed::reading_line_tip(line))
                                       : to_wide(row.tip);
            cell.left = left;
            cell.right = left + row_width;
            cell.top = top;
            cell.bottom = top + line_height;
            g_state.hover_cells.push_back(cell);
            left = cell.right;
            if (index + 1 < line.segments.size()) {
                left += segment_width(L" · ", plan.font);
            }
        }
        top += line_height;
    }
}

// ---------- 悬停提示 ----------

void destroy_tooltip() {
    if (g_tooltip != nullptr) {
        ::DestroyWindow(g_tooltip);
        g_tooltip = nullptr;
    }
}

// TOOLINFOW::cbSize 要按**本进程里实际生效的** comctl32 版本填：旧版本只认较小的结构，填大了
// TTM_ADDTOOL 会失败，悬停就永远不出现。所以按候选值从大到小各试一次。
bool create_tooltip(HWND owner) {
    if (g_tooltip != nullptr) {
        return true;
    }
    g_tooltip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                  WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr,
                                  ::GetModuleHandleW(nullptr), nullptr);
    if (g_tooltip == nullptr) {
        log_format(L"cannot create the hover tooltip: %s", last_error_text().c_str());
        return false;
    }
    ::SendMessageW(g_tooltip, TTM_SETMAXTIPWIDTH, 0, kHoverMaxTipWidth);

    TOOLINFOW tool{};
    HINSTANCE const instance = ::GetModuleHandleW(nullptr);
    bool added = false;
    for (size_t size = sizeof(TOOLINFOW); size >= sizeof(TOOLINFOW) - 8; size -= 4) {
        tool = TOOLINFOW{};
        tool.cbSize = static_cast<UINT>(size);
        // **不用 TTF_SUBCLASS**：那会让 comctl32 去子类化任务栏窗口，等于替换 Shell_TrayWnd 的
        // 窗口过程（实测约十秒后 explorer 崩溃）。悬停完全由 XAML 的指针事件驱动，我们只手动
        // TTM_TRACKACTIVATE / TTM_TRACKPOSITION。
        tool.uFlags = 0;
        tool.hwnd = owner;
        tool.uId = 1;
        tool.hinst = instance;
        tool.lpszText = const_cast<LPWSTR>(L"");
        tool.rect = RECT{0, 0, 0, 0};
        if (::SendMessageW(g_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)) != FALSE) {
            added = true;
            log_format(L"hover tooltip ready (TOOLINFOW::cbSize=%zu)", size);
            break;
        }
    }
    if (!added) {
        log_format(L"TTM_ADDTOOL failed for every cbSize candidate: %s", last_error_text().c_str());
        destroy_tooltip();
        return false;
    }
    return true;
}

void show_tip(std::wstring const& text, POINT screen_at) {
    if (g_tooltip == nullptr || text.empty()) {
        return;
    }
    TOOLINFOW tool{};
    tool.cbSize = sizeof(TOOLINFOW);
    tool.hwnd = g_state.tray;
    tool.uId = 1;
    tool.lpszText = const_cast<LPWSTR>(text.c_str());
    ::SendMessageW(g_tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
    ::SendMessageW(g_tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(screen_at.x, screen_at.y));
    ::SendMessageW(g_tooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tool));
}

void hide_tip() {
    if (g_tooltip == nullptr) {
        return;
    }
    TOOLINFOW tool{};
    tool.cbSize = sizeof(TOOLINFOW);
    tool.hwnd = g_state.tray;
    tool.uId = 1;
    ::SendMessageW(g_tooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tool));
    g_state.hover_last_hit = -2;
}

// ---------- 菜单（自己一条线程：TrackPopupMenu 内部是模态消息循环） ----------

std::mutex g_menu_mutex;
HANDLE g_menu_thread = nullptr;
DWORD g_menu_thread_id = 0;
HWND g_menu_owner = nullptr;
bool g_menu_running = false;

constexpr wchar_t kMenuOwnerClass[] = L"DeskFeedReadingMenuOwner";
constexpr unsigned kMenuRefreshId = 1;
constexpr unsigned kMenuShowWidgetId = 2;

DWORD WINAPI menu_thread_entry(LPVOID parameter);

void start_menu(int x, int y) {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    if (g_menu_running || g_menu_thread != nullptr) {
        return;  // 合并：菜单已经开着就不再开第二个
    }
    g_menu_running = true;
    g_menu_thread = ::CreateThread(nullptr, 0, menu_thread_entry,
                                   reinterpret_cast<LPVOID>(static_cast<INT_PTR>(
                                       (static_cast<long long>(y) << 32) | static_cast<unsigned>(x))),
                                   0, &g_menu_thread_id);
    if (g_menu_thread == nullptr) {
        g_menu_running = false;
        log_format(L"cannot start the menu thread: %s", last_error_text().c_str());
    }
}

DWORD WINAPI menu_thread_entry(LPVOID parameter) {
    auto const packed = static_cast<long long>(reinterpret_cast<INT_PTR>(parameter));
    int const x = static_cast<int>(static_cast<unsigned>(packed & 0xFFFFFFFF));
    int const y = static_cast<int>(packed >> 32);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = ::DefWindowProcW;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.lpszClassName = kMenuOwnerClass;
    ::RegisterClassExW(&window_class);

    // 属主窗口必须由**这条线程自己**创建：跨线程的属主会让 TrackPopupMenu 立刻以
    // ERROR_INVALID_PARAMETER(87) 失败。
    HWND const owner = ::CreateWindowExW(WS_EX_TOOLWINDOW, kMenuOwnerClass, L"DeskFeed", WS_POPUP,
                                         x, y, 1, 1, nullptr, nullptr,
                                         ::GetModuleHandleW(nullptr), nullptr);
    if (owner != nullptr) {
        {
            std::lock_guard<std::mutex> guard(g_menu_mutex);
            g_menu_owner = owner;
        }
        HMENU const menu = ::CreatePopupMenu();
        ::AppendMenuW(menu, MF_STRING, kMenuRefreshId, L"立即刷新读数");
        ::AppendMenuW(menu, MF_STRING, kMenuShowWidgetId, L"打开桌面悬浮窗");
        ::SetForegroundWindow(owner);
        UINT const chosen = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, owner,
                                             nullptr);
        // 官方 pattern：SetForegroundWindow(owner) → TrackPopupMenu → PostMessage(owner, WM_NULL)。
        // 最后这一下是让菜单真的收干净（否则它会留着"最后一次菜单"的状态影响下一次弹出）。
        ::PostMessageW(owner, WM_NULL, 0, 0);
        ::DestroyMenu(menu);
        if (chosen == kMenuRefreshId) {
            action_worker_post_refresh();
        } else if (chosen == kMenuShowWidgetId) {
            action_worker_post_show_widget();
        }
        ::DestroyWindow(owner);
    } else {
        log_format(L"cannot create the menu owner window: %s", last_error_text().c_str());
    }

    {
        std::lock_guard<std::mutex> guard(g_menu_mutex);
        g_menu_owner = nullptr;
        g_menu_running = false;
    }
    return 0;
}

// 只找**本进程**、而且属于那个菜单线程的弹出菜单窗口。`FindWindowW(L"#32768")` 会拿到桌面上
// 任意一个进程的菜单：打空自己的还算轻，把别人正开着的右键菜单关掉就是跨进程副作用。
struct MenuSearch {
    DWORD process_id = 0;
    DWORD thread_id = 0;
    HWND found = nullptr;
};

BOOL CALLBACK find_own_menu_window(HWND window, LPARAM param) {
    auto* search = reinterpret_cast<MenuSearch*>(param);
    DWORD owner_pid = 0;
    DWORD const owner_thread = ::GetWindowThreadProcessId(window, &owner_pid);
    if (owner_pid != search->process_id) {
        return TRUE;
    }
    if (search->thread_id != 0 && owner_thread != search->thread_id) {
        return TRUE;
    }
    search->found = window;
    return FALSE;  // 找到了，停
}

// 收菜单：先把本进程的弹出菜单窗口用 ESC 请走，再等线程句柄。返回 false = 线程还没退出，
// 调用方必须保留模块。
bool close_menu(unsigned timeout_ms) {
    HANDLE thread = nullptr;
    HWND owner = nullptr;
    DWORD menu_thread_id = 0;
    {
        std::lock_guard<std::mutex> guard(g_menu_mutex);
        thread = g_menu_thread;
        owner = g_menu_owner;
        menu_thread_id = g_menu_thread_id;
    }
    if (thread == nullptr) {
        return true;
    }
    if (owner != nullptr) {
        // 菜单是负载自己线程上的模态 TrackPopupMenu：它只认键盘/点击，认不了窗口消息。所以找到
        // **属于本进程、且属于那个菜单线程**的 Win32 弹出菜单窗口（类名 #32768）并给它 ESC；
        // 找不到就退回 owner 的 WM_CANCELMODE（那条本来就是我们自己的窗口）。
        MenuSearch search;
        search.process_id = ::GetCurrentProcessId();
        search.thread_id = menu_thread_id;
        ::EnumWindows(&find_own_menu_window, reinterpret_cast<LPARAM>(&search));
        if (search.found != nullptr) {
            ::PostMessageW(search.found, WM_KEYDOWN, VK_ESCAPE, 0);
            ::PostMessageW(search.found, WM_KEYUP, VK_ESCAPE, 0);
        } else {
            ::PostMessageW(owner, WM_CANCELMODE, 0, 0);
        }
    }
    if (::WaitForSingleObject(thread, timeout_ms) != WAIT_OBJECT_0) {
        log_format(L"the menu thread did not exit within %u ms", timeout_ms);
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(g_menu_mutex);
        ::CloseHandle(g_menu_thread);
        g_menu_thread = nullptr;
        g_menu_running = false;
    }
    return true;
}

// ---------- 交互 ----------

void on_pointer_moved(wuxc::Border const& block, wuxi::PointerRoutedEventArgs const& args) {
    if (g_tooltip == nullptr || g_state.hover_cells.empty()) {
        return;
    }
    try {
        auto const point = args.GetCurrentPoint(block);
        double const x = point.Position().X;
        double const y = point.Position().Y;

        if (!g_state.hover_origin_known) {
            // 块的屏幕原点：用"指针屏幕位置 − 指针在块内的 DIP 位置"反推，一次就够。
            POINT cursor{};
            if (::GetCursorPos(&cursor) != FALSE) {
                double const scale = static_cast<double>(window_dpi(g_state.tray)) / 96.0;
                g_state.hover_origin.x = static_cast<LONG>(cursor.x - x * scale);
                g_state.hover_origin.y = static_cast<LONG>(cursor.y - y * scale);
                g_state.hover_origin_known = true;
            }
        }

        int hit = -1;
        for (size_t index = 0; index < g_state.hover_cells.size(); ++index) {
            if (g_state.hover_cells[index].hit(x, y)) {
                hit = static_cast<int>(index);
                break;
            }
        }
        if (hit == g_state.hover_last_hit) {
            return;
        }
        g_state.hover_last_hit = hit;

        std::wstring const& text = hit >= 0 ? g_state.hover_cells[static_cast<size_t>(hit)].tip
                                            : g_state.hover_detail;
        if (text.empty() || g_tooltip == nullptr) {
            hide_tip();
            return;
        }
        POINT cursor{};
        ::GetCursorPos(&cursor);
        cursor.x += 12;
        cursor.y += 20;
        show_tip(text, cursor);
    } catch (winrt::hresult_error const& failure) {
        log_format(L"hover handling threw %s", hresult_text(failure.code()).c_str());
    } catch (...) {
        log_line(L"hover handling threw an unknown exception");
    }
}

void attach_interaction(wuxc::Border const& block) {
    try {
        // 背景必须是**透明但非 null** 的刷子：XAML 的命中测试要求非 null 背景，否则只有字形像素
        // 才吃指针，双击与右键大部分时候收不到。
        block.Background(wuxm::SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0, 0, 0, 0)));
        block.PointerMoved([block](winrt::Windows::Foundation::IInspectable const&,
                                   wuxi::PointerRoutedEventArgs const& args) {
            on_pointer_moved(block, args);
        });
        block.PointerExited([block](winrt::Windows::Foundation::IInspectable const&,
                                    wuxi::PointerRoutedEventArgs const&) { hide_tip(); });
        // 双击 = 把桌面悬浮窗叫出来（与旧交互一致）。
        block.DoubleTapped([](winrt::Windows::Foundation::IInspectable const&,
                              wuxi::DoubleTappedRoutedEventArgs const&) {
            action_worker_post_show_widget();
        });
        // 右键 = 本模块自己的 Win32 弹出菜单。不是 XAML 的 Flyout：那块住在任务栏岛里，XAML 自己
        // 的下拉会被岛裁掉。
        block.RightTapped([block](winrt::Windows::Foundation::IInspectable const&,
                                  wuxi::RightTappedRoutedEventArgs const&) {
            POINT cursor{};
            if (::GetCursorPos(&cursor) != FALSE) {
                start_menu(cursor.x, cursor.y);
            }
        });
    } catch (winrt::hresult_error const& failure) {
        log_format(L"cannot attach the interaction handlers: %s", hresult_text(failure.code()).c_str());
    }
}

// ---------- 插入 / 更新 / 摘除 ----------

bool insert_into(wux::FrameworkElement const& tray_element, deskfeed::ReadingContent const& content,
                 Colors const& colors, ReadingPlan const& plan, double max_width, std::wstring& error) {
    g_state.block = wuxc::Border();
    g_state.block.Name(kBlockName);
    g_state.label = wuxc::TextBlock();
    g_state.label.Name(kReadingName);
    g_state.label.FontSize(plan.font);
    // 常规字重：前缀与数值只用取色区分。
    g_state.label.FontWeight(winrt::Windows::UI::Text::FontWeights::Normal());
    g_state.label.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    g_state.label.MaxWidth(max_width);

    std::vector<Segment> const segments = reading_segments(content, colors, plan.per_line);
    apply_segments(g_state.label, segments);
    g_state.block.Child(g_state.label);
    attach_interaction(g_state.block);
    // 悬停提示的属主窗口：任务栏窗口，自测宿主里是宿主自己的窗口（没有 Shell_TrayWnd）。
    create_tooltip(g_state.tray != nullptr ? g_state.tray : g_state.host_window);

    if (auto grid = tray_element.try_as<wuxc::Grid>()) {
        g_state.column_owner = grid;
        g_state.column = wuxc::ColumnDefinition();
        g_state.column.Width(wux::GridLengthHelper::Auto());
        grid.ColumnDefinitions().Append(g_state.column);

        unsigned const columns = grid.ColumnDefinitions().Size();
        g_state.shifted.clear();
        unsigned const children = grid.Children().Size();
        for (unsigned index = 0; index < children; ++index) {
            if (auto child = grid.Children().GetAt(index).try_as<wux::FrameworkElement>()) {
                grid.SetColumn(child, grid.GetColumn(child) + 1);
                g_state.shifted.push_back(child);
            }
        }
        grid.Children().Append(g_state.block);
        grid.SetColumn(g_state.block, 0);
        log_format(L"inserted the reading block into %s (columns=%u, shifted %zu child(ren))",
                   kTrayElementName, columns, g_state.shifted.size());
    } else if (auto panel = tray_element.try_as<wuxc::StackPanel>()) {
        unsigned const children = panel.Children().Size();
        panel.Children().InsertAt(0, g_state.block);
        log_format(L"inserted the reading block into %s (stack panel, %u existing child(ren))",
                   kTrayElementName, children);
    } else {
        error = L"托盘元素既不是 Grid 也不是 StackPanel";
        g_state.block = nullptr;
        g_state.label = nullptr;
        return false;
    }

    try {
        // 刻意**不**调 UpdateLayout：XAML 的布局是异步的，催布局等于在任务栏自己的布局过程里插
        // 一脚。悬停命中用的是离树测量出来的宽度，不依赖这里的实际尺寸；结算后的值由下一轮日志给出。
        (void)0;
    } catch (winrt::hresult_error const&) {
    }
    rebuild_hover_model(content, plan, plan.width, plan.height);
    g_block_present.store(true);
    return true;
}

void remove_block() {
    if (g_state.block != nullptr) {
        if (auto parent = g_state.block.Parent().try_as<wuxc::Panel>()) {
            uint32_t index = 0;
            bool const found = parent.Children().IndexOf(g_state.block, index);
            if (found && index < parent.Children().Size()) {
                parent.Children().RemoveAt(index);
            }
        }
        if (g_state.column_owner != nullptr && g_state.column != nullptr) {
            for (wux::FrameworkElement const& child : g_state.shifted) {
                unsigned const column = g_state.column_owner.GetColumn(child);
                if (column > 0) {
                    g_state.column_owner.SetColumn(child, column - 1);
                }
            }
            uint32_t column_index = 0;
            bool const found =
                g_state.column_owner.ColumnDefinitions().IndexOf(g_state.column, column_index);
            if (found && column_index < g_state.column_owner.ColumnDefinitions().Size()) {
                g_state.column_owner.ColumnDefinitions().RemoveAt(column_index);
            }
        }
    }
    g_state.block = nullptr;
    g_state.label = nullptr;
    g_state.column_owner = nullptr;
    g_state.column = nullptr;
    g_state.shifted.clear();
    g_state.runs.clear();
    g_state.applied.clear();
    g_state.hover_cells.clear();
    g_state.hover_detail.clear();
    g_state.hover_last_hit = -2;
    g_block_present.store(false);
    hide_tip();
}

bool update_block(deskfeed::ReadingContent const& content, Colors const& colors, double max_width,
                  std::wstring& error) {
    if (g_state.block == nullptr || g_state.label == nullptr) {
        error = L"块不在树里";
        return false;
    }
    ReadingPlan const plan = plan_reading(content, colors, max_width, available_height(g_state.tray_element));
    std::vector<Segment> const segments = reading_segments(content, colors, plan.per_line);
    if (same_segments(segments, g_state.applied)) {
        // 段序列没变：只改已经持有的 Run 的文字与前景色，Inlines 一个元素都不动。
        size_t run_index = 0;
        for (size_t index = 0; index < segments.size(); ++index) {
            if (segments[index].line_break) {
                continue;
            }
            if (run_index >= g_state.runs.size()) {
                break;
            }
            g_state.runs[run_index].Text(segments[index].text);
            g_state.runs[run_index].Foreground(wuxm::SolidColorBrush(segments[index].color));
            ++run_index;
        }
        if (g_state.label.FontSize() != plan.font) {
            g_state.label.FontSize(plan.font);
        }
    } else {
        apply_segments(g_state.label, segments);
        g_state.label.FontSize(plan.font);
        g_state.label.MaxWidth(max_width);
    }
    rebuild_hover_model(content, plan, plan.width, plan.height);
    log_format(L"reading block updated (rows=%zu per_line=%zu lines=%zu font=%.1f)",
               content.rows.size(), plan.per_line, plan.lines, plan.font);
    return true;
}

bool current_content(deskfeed::ReadingContent& out) {
    std::lock_guard<std::mutex> guard(g_content_mutex);
    if (!g_have_content) {
        return false;
    }
    out = g_content;
    return true;
}

bool resolve_tray_element(winrt::Windows::Foundation::IInspectable& element, std::wstring& error) {
    if (g_diagnostics_closed.load()) {
        error = L"诊断连接已经关掉，无法再解析元素";
        return false;
    }
    std::vector<XamlTapSite> const sites = xaml_tap_sites();
    if (sites.empty()) {
        error = L"没有 XAML 核心交出诊断站点";
        return false;
    }
    size_t chosen = 0;
    for (size_t index = 0; index < sites.size(); ++index) {
        if (g_state.tray != nullptr && sites[index].thread_id == ::GetWindowThreadProcessId(g_state.tray, nullptr)) {
            chosen = index;
            break;
        }
    }
    g_state.site_index = chosen;
    if (!xaml_tap_resolve(chosen, element, error)) {
        return false;
    }
    return true;
}

// 任务栏线程上的动作体：解析（首次）/ 应用 / 摘除。它由 hook_post 在 T 上调用，异常边界在
// hook_post 里；这里只做工作。
bool apply_task(bool allow_placeholder, bool& visible, std::wstring& error) {
    if (g_state.slow_apply_ms != 0) {
        ::Sleep(g_state.slow_apply_ms);
    }

    deskfeed::ReadingContent content;
    if (!current_content(content)) {
        if (!allow_placeholder) {
            error = L"还没有可显示的读数";
            return false;
        }
        deskfeed::ReadingRow row;
        row.label = "DeskFeed";
        row.value = "no data yet";
        row.tone = deskfeed::ReadingTone::Normal;
        content.rows.push_back(row);
    }

    Colors const colors = colors_for(light_taskbar(g_state.tray_element));
    double const max_width = block_max_width();

    if (content.rows.empty()) {
        // 没有可显示的条目：把块摘掉（整块隐藏），之后读数回来时会用已经持有的元素补插。
        if (g_state.block != nullptr) {
            remove_block();
        }
        visible = false;
        return true;
    }

    if (g_state.tray_element == nullptr) {
        winrt::Windows::Foundation::IInspectable element{nullptr};
        if (!resolve_tray_element(element, error)) {
            return false;
        }
        g_state.tray_element = element.try_as<wux::FrameworkElement>();
        if (g_state.tray_element == nullptr) {
            error = L"解析出来的元素不是 FrameworkElement";
            return false;
        }
        g_tray_element_held.store(true);
        log_format(L"resolved %s (held for the whole run; the diagnostics are closed right after this)",
                   winrt::get_class_name(g_state.tray_element).c_str());
    }

    if (g_state.block == nullptr) {
        ReadingPlan const plan =
            plan_reading(content, colors, max_width, available_height(g_state.tray_element));
        if (!insert_into(g_state.tray_element, content, colors, plan, max_width, error)) {
            return false;
        }
        visible = true;
        return true;
    }

    if (!update_block(content, colors, max_width, error)) {
        return false;
    }
    visible = true;
    return true;
}

bool remove_task(bool& faulted) {
    (void)faulted;
    remove_block();
    destroy_tooltip();
    // 摘除是收尾动作，之后不会再插回去：把持有的托盘元素引用也在这里放掉（元素属于任务栏自己的
    // 树，任务栏自己还持有它，这里放的只是本模块那一份）。
    g_state.tray_element = nullptr;
    g_tray_element_held.store(false);
    return true;
}

// hook_post 的执行体：**没有异常可以穿出去**（hook_post 里有 SEH + C++ 双边界）。
bool execute_hook_action(void* context, HookAction action, std::wstring& error) {
    (void)context;
    bool visible = false;
    if (action == HookAction::Apply) {
        return apply_task(g_state.probe_placeholder, visible, error);
    }
    bool faulted = false;
    return remove_task(faulted);
}

}  // namespace

bool taskbar_xaml_watch(std::wstring& error) {
    if (xaml_tap_sites().empty()) {
        error = L"没有 XAML 核心交出诊断站点";
        return false;
    }
    // 走查只挑一个站点（任务栏那条线程上的核心优先），**不重试**：重试就是又一次走查。
    size_t chosen = 0;
    std::vector<XamlTapSite> const sites = xaml_tap_sites();
    if (g_state.tray != nullptr) {
        DWORD const tray_thread = ::GetWindowThreadProcessId(g_state.tray, nullptr);
        for (size_t index = 0; index < sites.size(); ++index) {
            if (sites[index].thread_id == tray_thread) {
                chosen = index;
                break;
            }
        }
    }
    g_state.site_index = chosen;
    return xaml_tap_watch(chosen, kTrayElementName, error);
}

bool taskbar_xaml_attach(std::wstring const& scope, std::wstring& error) {
    (void)scope;
    hook_post_set_executor(&execute_hook_action, nullptr);
    g_state.tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (g_state.tray == nullptr) {
        g_state.tray = ::FindWindowW(L"Shell_SecondaryTrayWnd", nullptr);
    }
    if (g_state.tray == nullptr) {
        error = L"找不到 Shell_TrayWnd";
        return false;
    }
    DWORD const tray_thread = ::GetWindowThreadProcessId(g_state.tray, nullptr);
    if (tray_thread == 0) {
        error = L"找不到任务栏线程";
        return false;
    }

    // composition 规避必须在真正连接诊断之前装好：XAML 读完之后会缓存那个结论，装晚了就没用。
    // 失败只记日志——退化成不做规避的旧行为，而不是不插块。
    std::wstring hook_error;
    if (reg_hook_install(hook_error)) {
        std::wstring detail;
        bool const ok = reg_hook_probe_read(detail);
        log_format(L"reg hook: selftest %s: %s", ok ? L"passed" : L"FAILED", detail.c_str());
    } else {
        log_format(L"reg hook: not installed (%s); the XAML composition walker stays enabled",
                   hook_error.c_str());
    }

    if (!xaml_tap_start(error)) {
        // 连接失败也要把导入表恢复回去：规避钩子没有用了，不能留在进程里。
        std::wstring restore_error;
        if (!reg_hook_restore(restore_error)) {
            log_format(L"reg hook: restore failed: %s", restore_error.c_str());
        }
        return false;
    }

    for (int attempt = 0; attempt < kSiteAttempts && xaml_tap_sites().empty(); ++attempt) {
        ::Sleep(kSiteDelayMs);
    }
    // 连接过程里 XAML 可能又载入了模块，补扫一次导入表。
    reg_hook_rescan();

    bool const watched = taskbar_xaml_watch(error);

    // 解析 + 插入都在任务栏线程上做（元素有线程亲和性）。
    if (watched) {
        HookPostResult const posted = hook_post_run(tray_thread, HookAction::Apply, kPostTimeoutMs);
        if (!posted.ok) {
            error = posted.error.empty() ? L"任务栏线程没有应用读数" : posted.error;
        }
    }

    // 解析阶段到此结束：诊断这条线整条关掉（成功、失败、没走成都关），并且**立刻**恢复导入表——
    // 那层规避只需要覆盖走查这一段。
    xaml_tap_release_walk(g_state.site_index);
    std::wstring tap_detail;
    if (!xaml_tap_stop(2000, tap_detail)) {
        log_format(L"diagnostics teardown is incomplete: %s", tap_detail.c_str());
    }
    g_diagnostics_closed.store(true);

    std::wstring restore_error;
    if (!reg_hook_restore(restore_error)) {
        log_format(L"reg hook: restore failed: %s", restore_error.c_str());
    }

    g_attached.store(watched && g_block_present.load());
    return watched && g_block_present.load();
}

bool taskbar_xaml_request_update(deskfeed::ReadingContent const& content) {
    {
        std::lock_guard<std::mutex> guard(g_content_mutex);
        g_content = content;
        g_have_content = true;
    }
    if (g_state.tray == nullptr) {
        return false;
    }
    // 运行期绝不走查，也绝不重连诊断：诊断连接在解析阶段就已经关掉。块还不在树里时只能用已经
    // 持有的托盘元素补插；连元素都没有（attach 没成功）就什么都不做。
    if (!g_block_present.load() && !g_tray_element_held.load()) {
        log_line(L"reading: update dropped (no tray element is held and the diagnostics are closed)");
        return false;
    }

    DWORD const tray_thread = ::GetWindowThreadProcessId(g_state.tray, nullptr);
    HookPostResult const posted = hook_post_run(tray_thread, HookAction::Apply, kPostTimeoutMs);
    if (!posted.ok) {
        log_format(L"reading: update failed: %s", posted.error.c_str());
        return false;
    }
    return true;
}

bool taskbar_xaml_detach(unsigned timeout_ms, std::wstring& detail) {
    bool safe = true;

    // 第一步收菜单：菜单是负载自己线程上的模态 TrackPopupMenu，还开着就拆除会留下"块没了、菜单
    // 还在"。线程没退出就是没退休。
    if (!close_menu(timeout_ms)) {
        safe = false;
        detail = L"菜单线程没有在期限内退出";
    }

    // 第三步：摘块并销毁悬停窗口（都在任务栏线程上）。
    bool const need_remove = g_block_present.load() || g_tooltip != nullptr;
    if (need_remove && g_state.tray != nullptr && !g_state.probe_inline) {
        DWORD const tray_thread = ::GetWindowThreadProcessId(g_state.tray, nullptr);
        HookPostResult const posted = hook_post_run(tray_thread, HookAction::Remove, timeout_ms);
        if (!posted.ok && (g_block_present.load() || g_tooltip != nullptr)) {
            safe = false;
            if (!detail.empty()) {
                detail += L"; ";
            }
            detail += L"任务栏线程没有摘掉读数块";
        }
    } else if (need_remove) {
        // 自测宿主：调用线程就是树所属的那条线程，直接做。
        bool faulted = false;
        remove_task(faulted);
    }

    // 钩子的静止证明（完成事件 + 对 T 的窗口做一次非排队 ping），拿到才归还自引用。
    std::wstring hook_detail;
    bool const hook_proven =
        hook_post_retire(g_ack_window_override,
                         g_state.tray != nullptr
                             ? ::GetWindowThreadProcessId(g_state.tray, nullptr)
                             : 0,
                         timeout_ms, hook_detail);
    if (!hook_proven) {
        safe = false;
        if (!detail.empty()) {
            detail += L"; ";
        }
        detail += hook_detail;
    }

    // 只有 ping 证明拿到之后才允许丢掉"确认窗口属于哪条 T"这份身份：失败时保留它，下一次重试才
    // 能对同一条 T 再发一次 ack。这里只是把指针放下，绝不从工作线程去动 XAML 结构。
    if (hook_proven) {
        g_state.tray = nullptr;
    }
    g_attached.store(false);
    return safe;
}

bool taskbar_xaml_block_visible() { return g_block_present.load(); }

void taskbar_xaml_probe_set_slow_apply(unsigned ms) { g_state.slow_apply_ms = ms; }

// 生产与自测共用的入口：只把读数写进内容槽（加锁），不碰 XAML 结构。生产路径由负载线程在读到
// 第一帧之后调用它，所以它**不是**"自测宿主专用"，也不许被整段 guard 掉。
void taskbar_xaml_set_reading(deskfeed::ReadingContent const& content) {
    std::lock_guard<std::mutex> guard(g_content_mutex);
    g_content = content;
    g_have_content = true;
}

// 自测宿主：调用线程就是树所属的线程，所以"解析 + 应用"直接在本线程做。
bool taskbar_xaml_probe_run(bool dump_only, bool allow_placeholder, bool& visible,
                            std::wstring& error) {
    visible = false;
    g_state.probe_placeholder = allow_placeholder;
    g_state.probe_inline = true;
    hook_post_set_executor(&execute_hook_action, nullptr);

    if (dump_only) {
        winrt::Windows::Foundation::IInspectable element{nullptr};
        if (g_diagnostics_closed.load() || !resolve_tray_element(element, error)) {
            xaml_tap_release_walk(g_state.site_index);
            return false;
        }
        g_state.tray_element = element.try_as<wux::FrameworkElement>();
        g_tray_element_held.store(g_state.tray_element != nullptr);
        log_format(L"resolved %s", winrt::get_class_name(g_state.tray_element).c_str());
        xaml_tap_release_walk(g_state.site_index);
        return true;
    }

    bool const ok = apply_task(allow_placeholder, visible, error);
    if (ok && !dump_only) {
        xaml_tap_release_walk(g_state.site_index);
    }
    return ok;
}

int taskbar_xaml_probe_hover_cells(long* out, int capacity) {
    if (out == nullptr || capacity < 4 || g_state.block == nullptr || !g_state.hover_origin_known) {
        return 0;
    }
    int written = 0;
    double const scale = static_cast<double>(window_dpi(g_state.tray)) / 96.0;
    out[written++] = g_state.hover_origin.x;
    out[written++] = g_state.hover_origin.y;
    out[written++] = static_cast<long>(g_state.hover_width * scale);
    out[written++] = static_cast<long>(g_state.hover_height * scale);
    for (HoverCell const& cell : g_state.hover_cells) {
        if (written + 4 > capacity) {
            break;
        }
        out[written++] = static_cast<long>(g_state.hover_origin.x + cell.left * scale);
        out[written++] = static_cast<long>(g_state.hover_origin.y + cell.top * scale);
        out[written++] = static_cast<long>(g_state.hover_origin.x + cell.right * scale);
        out[written++] = static_cast<long>(g_state.hover_origin.y + cell.bottom * scale);
    }
    return written;
}

void taskbar_xaml_set_probe_inline(bool inline_mode) { g_state.probe_inline = inline_mode; }

void taskbar_xaml_set_host_window(HWND window) {
    g_state.host_window = window;
    // 钩子收尾时那次"帧已弹掉"的 ping 需要一个**属于任务栏线程**的窗口。真实注入里是
    // Shell_TrayWnd；自测宿主没有它，就由宿主交一个自己的、user32 类的窗口（例如 STATIC），
    // 绝不能用本模块自己的窗口过程当确认目标。
    hook_post_set_ack_window(window);
}

}  // namespace deskfeed::payload
