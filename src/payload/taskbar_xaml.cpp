#include "payload/taskbar_xaml.h"

#include "core/utf8.h"
#include "display/taskbar_layout.h"
#include "payload/action_worker.h"
#include "payload/hook_post.h"
#include "payload/reg_hook.h"
#include "payload/xaml_tap.h"
#include "platform/log.h"
#include "platform/screens.h"
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
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <memory>
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

    // 这一块状态对应的屏幕与任务栏线程。同一块任务栏上的所有操作都必须回到 thread_id。
    std::wstring screen_id;
    std::wstring label;
    DWORD thread_id = 0;
    bool primary = false;
    bool available = false;  // 托盘元素已经解析出来（此后才允许插入）
    bool enabled = false;    // 本次实际要显示（配置命中或退化命中）

    // 插进去的 XAML 对象。只在任务栏线程上被碰，放在这里是为了活过插入调用。
    wuxc::Border block{nullptr};
    wuxc::TextBlock label_element{nullptr};
    wuxc::Grid column_owner{nullptr};
    wuxc::ColumnDefinition column{nullptr};
    std::vector<wux::FrameworkElement> shifted;
    std::vector<wuxd::Run> runs;
    std::vector<Segment> applied;

    // 解析出来的托盘面板，**长期持有**：句柄只来自那唯一一次走查，解析一次之后诊断连接就关掉了，
    // 所以它是运行期唯一能碰的任务栏 XAML 入口（补插、验证都用它）。只在任务栏线程上被碰。
    wux::FrameworkElement tray_element{nullptr};
    bool tray_element_held = false;

    // 悬停提示属于具体那一块任务栏（两个屏幕同时显示时各弹各的，互不覆盖）。只在任务栏线程上碰。
    HWND tooltip = nullptr;

    // 悬停命中模型：每个条目在块内的矩形与它**自己**的 tip（为空表示这一条没有可显示的提示）。
    std::vector<HoverCell> hover_cells;
    // 提示框此刻**已经显示**的是哪一条、哪份文字：`hover_shown_hit < 0` 表示现在什么都没显示。
    // 这一对是"同一条目、同一份文字"的唯一判据——内容重建（`rebuild_hover_model`）刻意不清它们，
    // 否则每刷新一次读数都会让下一次指针移动重新弹一次提示框，看上去就是提示框跟着光标跳。
    int hover_shown_hit = -1;
    std::wstring hover_shown_tip;
    double hover_width = 0.0;
    double hover_height = 0.0;
    double hover_font = 0.0;
    POINT hover_origin{};
    bool hover_origin_known = false;

    // 自测宿主专用。
    unsigned slow_apply_ms = 0;
    bool probe_placeholder = false;
    bool probe_inline = false;
    // 自测宿主：DeskFeedProbeWatch 走查到的句柄（还没有解析成对象）。
    std::vector<std::uint64_t> watched_handles;

    // 状态查询线程只能读原子量，绝不碰 XAML 对象。
    std::atomic<bool> block_present{false};
};

// 目标表只由负载线程（或自测宿主的 UI 线程）改写；状态查询用全局聚合量，不遍历它。
std::vector<std::unique_ptr<State>> g_targets;

// 当前正在被操作的那一块。负载线程把动作投递到任务栏线程后，执行体在 T 上按 context 设置它；
// 任务栏自己的悬停/右键回调也会在进入时把它设成自己的那一块。用 thread_local 保证两条线程
// 各自看到自己的目标，不靠一个跨线程共享的裸指针。
thread_local State* g_current = nullptr;

State& current_state() { return *g_current; }
// 下面的绘制/悬停/菜单辅助函数都通过 `g_state` 访问“当前这一块”；它是 thread_local 的引用，
// 不是跨线程共享的全局对象。进入任务栏回调或一次投递时由 CurrentTargetGuard 设置。
#define g_state current_state()

// 显示内容的进程内拷贝：负载线程写、任务栏线程读。
std::mutex g_content_mutex;
deskfeed::ReadingContent g_content;
bool g_have_content = false;

// 屏幕配置：与显示帧里的 `screens_configured` / `screens` 一一对应。负载线程写，菜单线程在
// 打开菜单时读一份快照，任务栏线程不碰它。
std::mutex g_screen_config_mutex;
bool g_screens_configured = false;
std::vector<std::wstring> g_enabled_screens;

std::atomic<bool> g_any_block_present{false};
std::atomic<bool> g_diagnostics_closed{false};
std::atomic<bool> g_attached{false};

// 负载线程上保存的“上一次真正应用成功的读数”。每 250 ms 的一轮空转不应该再走一次钩子投递。
ReadingContent g_last_applied_content;
bool g_have_applied_content = false;
bool g_last_degraded = false;

// 自测宿主的进程内开关（探针在 UI 线程直接调用，不经过任务栏线程）。
HWND g_probe_host_window = nullptr;
unsigned g_probe_slow_apply_ms = 0;
bool g_probe_inline_mode = false;
bool g_probe_placeholder = false;
#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
bool g_probe_override_mode = false;
#endif

// 走查得到的句柄：按站点索引存放。解析必须在任务栏线程上做（元素有线程亲和性），所以这里只
// 先记下句柄，随后由 HookAction::Resolve 在 T 上解析。
std::vector<std::vector<std::uint64_t>> g_watched_handles;

HWND g_ack_window_override = nullptr;

// 托盘右键菜单里的屏幕条目。菜单在**另一条线程**上弹出，所以这里保存一份快照；真正的目标表
// 只由负载线程改写。
struct MenuScreenEntry {
    std::wstring id;
    std::wstring label;
    bool checked = false;
};
std::mutex g_menu_model_mutex;
std::vector<MenuScreenEntry> g_menu_model;

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
// 自测宿主登记的本进程托盘目标（见 taskbar_xaml.h 的 `taskbar_xaml_probe_set_tray_override`）。
// 只有它非空、并且 `hook_post_probe_armed()` 为真时，`taskbar_xaml_attach` 才走"只认这个窗口"那条
// 分支；生产构建里这个变量与那条分支都不存在。
std::atomic<HWND> g_probe_tray_override{nullptr};
#endif

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
    int screen_width = ::GetSystemMetrics(SM_CXSCREEN);
    HWND const window = g_state.tray != nullptr ? g_state.tray : ::GetDesktopWindow();
    // 每块任务栏用自己的显示器宽度：副屏分辨率/缩放与主屏不同时，拿主屏宽度会把块量错。
    if (g_state.tray != nullptr) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        HMONITOR const monitor =
            ::MonitorFromWindow(g_state.tray, MONITOR_DEFAULTTONEAREST);
        if (monitor != nullptr && ::GetMonitorInfoW(monitor, &info) != FALSE) {
            screen_width = info.rcMonitor.right - info.rcMonitor.left;
        }
    }
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

void refresh_any_block_present() {
    bool any = false;
    for (auto const& target : g_targets) {
        if (target->block_present.load()) {
            any = true;
            break;
        }
    }
    g_any_block_present.store(any);
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

// 悬停提示的三个动作（定义在下面的"悬停提示"一节）。内容重建时也要在**指针静止**的情况下校正屏幕
// 上那份文字，所以先在这里给出声明。
void update_tip_text(std::wstring const& text);
void hide_tip();

void rebuild_hover_model(deskfeed::ReadingContent const& content, ReadingPlan const& plan,
                         double width, double height) {
    g_state.hover_cells.clear();
    g_state.hover_width = width;
    g_state.hover_height = height;
    g_state.hover_font = plan.font;
    // 这里**只**重建几何与每格自己的文字；"提示框正显示着哪一条"那份状态原样留着。读数换了内容但
    // 命中的还是同一条、文字也没变时，指针再移动也不会重新 show/重新定位（见 on_pointer_moved）。

    std::vector<deskfeed::ReadingLine> const lines = deskfeed::reading_pack(content, plan.per_line);
    // 行高必须**按同一次测量的实际高度**算：`plan.height` 是这整块文字量出来的总高度，除以行数
    // 就是每行占的高度。拿一段文字的**宽度**（`segment_width` 量的是 `DesiredSize().Width`）当
    // 行高会让纵向命中落到别的行上。
    double const line_height =
        plan.lines > 0 ? plan.height / static_cast<double>(plan.lines) : 0.0;

    double top = 0.0;
    for (deskfeed::ReadingLine const& line : lines) {
        double left = 0.0;
        for (size_t index = 0; index < line.segments.size(); ++index) {
            deskfeed::ReadingRow const& row = line.segments[index];
            std::wstring prefix = row.label.empty() ? std::wstring() : to_wide(row.label) + L" ";
            double const row_width =
                segment_width(prefix + to_wide(row.value), plan.font);
            HoverCell cell;
            // 每个格子只带**它自己那个条目**的提示：为空就是空，不借同一行其它条目的提示。
            cell.tip = to_wide(row.tip);
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

    // 指针可能一直不动（就不会再有 pointermove 来校正）：重建之后要就地核对屏幕上那份文字。
    // 索引仍是"当前显示的是第几条"（条目重排会让它落到别的条目上，那就当作刷新，允许）；那一条
    // 已经不在模型里、或它的 detail 变成了空，就立刻隐藏——绝不把旧内容长期留在屏幕上。
    int const shown_index = g_state.hover_shown_hit;
    if (shown_index >= 0) {
        if (shown_index >= static_cast<int>(g_state.hover_cells.size()) ||
            g_state.hover_cells[static_cast<size_t>(shown_index)].tip.empty()) {
            hide_tip();
        } else {
            std::wstring const& now_tip = g_state.hover_cells[static_cast<size_t>(shown_index)].tip;
            if (now_tip != g_state.hover_shown_tip) {
                update_tip_text(now_tip);
            }
        }
    }
}

// ---------- 悬停提示 ----------

void destroy_tooltip() {
    if (g_state.tooltip != nullptr) {
        ::DestroyWindow(g_state.tooltip);
        g_state.tooltip = nullptr;
    }
}

// TOOLINFOW::cbSize 要按**本进程里实际生效的** comctl32 版本填：旧版本只认较小的结构，填大了
// TTM_ADDTOOL 会失败，悬停就永远不出现。所以按候选值从大到小各试一次。
bool create_tooltip(HWND owner) {
    if (g_state.tooltip != nullptr) {
        return true;
    }
    g_state.tooltip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                  WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr,
                                  ::GetModuleHandleW(nullptr), nullptr);
    if (g_state.tooltip == nullptr) {
        log_format(L"cannot create the hover tooltip: %s", last_error_text().c_str());
        return false;
    }
    ::SendMessageW(g_state.tooltip, TTM_SETMAXTIPWIDTH, 0, kHoverMaxTipWidth);

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
        if (::SendMessageW(g_state.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)) != FALSE) {
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

// 命中条目变了（或刚从隐藏状态回到这一条）才走这里：换文字 + 定位 + 激活。这是**唯一**给提示框定
// 位置的地方，并把"现在显示的是哪一条、哪份文字"记进状态。
void show_tip(int hit, std::wstring const& text, POINT screen_at) {
    if (g_state.tooltip == nullptr || text.empty()) {
        return;
    }
    TOOLINFOW tool{};
    tool.cbSize = sizeof(TOOLINFOW);
    tool.hwnd = g_state.tray;
    tool.uId = 1;
    tool.lpszText = const_cast<LPWSTR>(text.c_str());
    ::SendMessageW(g_state.tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
    ::SendMessageW(g_state.tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(screen_at.x, screen_at.y));
    ::SendMessageW(g_state.tooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tool));
    g_state.hover_shown_hit = hit;
    g_state.hover_shown_tip = text;
}

// 还是同一条目，只是它自己的显示内容变了：只换文字，**绝不**重新定位、绝不重新激活。位置只在命中
// 条目变化时给一次，提示框弹出来之后就不再跟着光标走。
void update_tip_text(std::wstring const& text) {
    if (g_state.tooltip == nullptr || g_state.hover_shown_hit < 0 || text.empty()) {
        return;
    }
    TOOLINFOW tool{};
    tool.cbSize = sizeof(TOOLINFOW);
    tool.hwnd = g_state.tray;
    tool.uId = 1;
    tool.lpszText = const_cast<LPWSTR>(text.c_str());
    ::SendMessageW(g_state.tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
    g_state.hover_shown_tip = text;
}

void hide_tip() {
    // 状态先清：提示框窗口已经不在了（拆块之后）也必须记住"现在什么都没显示"，否则重新进入同一条
    // 目时会被那条早就不成立的缓存挡掉。
    g_state.hover_shown_hit = -1;
    g_state.hover_shown_tip.clear();
    if (g_state.tooltip == nullptr) {
        return;
    }
    TOOLINFOW tool{};
    tool.cbSize = sizeof(TOOLINFOW);
    tool.hwnd = g_state.tray;
    tool.uId = 1;
    ::SendMessageW(g_state.tooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tool));
}

// ---------- 菜单（自己一条线程：TrackPopupMenu 内部是模态消息循环） ----------

std::mutex g_menu_mutex;
// 每次弹出菜单都创建一条短命线程（TrackPopupMenu 是模态循环）。线程句柄留在这里，直到
// `reap_menu_threads_locked` 确认它真的退出——收尾要拿它证明线程已退出，不能只看
// `g_menu_running`（那只表示“菜单关了”，线程可能还没返回）。
std::vector<HANDLE> g_menu_threads;
DWORD g_menu_thread_id = 0;
HWND g_menu_owner = nullptr;
bool g_menu_running = false;

constexpr wchar_t kMenuOwnerClass[] = L"DeskFeedReadingMenuOwner";
constexpr unsigned kMenuRefreshId = 1;
constexpr unsigned kMenuShowWidgetId = 2;
constexpr unsigned kMenuScreenBase = 100;

// 当前这次弹出用的屏幕快照（由 start_menu 从 g_menu_model 复制）。菜单线程与收菜单都只通过
// g_menu_mutex 访问它。
std::vector<MenuScreenEntry> g_active_menu_entries;

DWORD WINAPI menu_thread_entry(LPVOID parameter);

// 回收已经退出的菜单线程句柄；还在跑的原样留着，交给收尾等。调用方必须持有 g_menu_mutex。
void reap_menu_threads_locked() {
    for (auto it = g_menu_threads.begin(); it != g_menu_threads.end();) {
        if (::WaitForSingleObject(*it, 0) == WAIT_OBJECT_0) {
            ::CloseHandle(*it);
            it = g_menu_threads.erase(it);
        } else {
            ++it;
        }
    }
}

void start_menu(int x, int y) {
    std::vector<MenuScreenEntry> entries;
    {
        std::lock_guard<std::mutex> guard(g_menu_model_mutex);
        entries = g_menu_model;
    }
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    reap_menu_threads_locked();
    // 只按“菜单是否开着”合并。这里**不能**再看线程句柄非空：上一轮菜单线程结束后句柄在收尾前
    // 仍然留着，按句柄判断会让菜单只能打开一次。
    if (g_menu_running) {
        return;
    }
    g_menu_running = true;
    g_active_menu_entries = std::move(entries);
    HANDLE const thread = ::CreateThread(
        nullptr, 0, menu_thread_entry,
        reinterpret_cast<LPVOID>(static_cast<INT_PTR>((static_cast<long long>(y) << 32) |
                                                     static_cast<unsigned>(x))),
        0, &g_menu_thread_id);
    if (thread == nullptr) {
        g_menu_running = false;
        g_active_menu_entries.clear();
        log_format(L"cannot start the menu thread: %s", last_error_text().c_str());
        return;
    }
    g_menu_threads.push_back(thread);
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
        std::vector<MenuScreenEntry> const entries = g_active_menu_entries;
        if (!entries.empty()) {
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            for (std::size_t index = 0; index < entries.size(); ++index) {
                MenuScreenEntry const& entry = entries[index];
                UINT const flags =
                    MF_STRING | (entry.checked ? static_cast<UINT>(MF_CHECKED) : 0u);
                ::AppendMenuW(menu, flags, kMenuScreenBase + static_cast<unsigned>(index),
                              entry.label.c_str());
            }
        }
        ::SetForegroundWindow(owner);
        log_format(L"menu: opened at %d,%d with %zu screen entry/entries", x, y, entries.size());
        UINT const chosen = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, owner,
                                             nullptr);
        log_format(L"menu: closed (chosen=%u)", chosen);
        // 官方 pattern：SetForegroundWindow(owner) → TrackPopupMenu → PostMessage(owner, WM_NULL)。
        // 最后这一下是让菜单真的收干净（否则它会留着"最后一次菜单"的状态影响下一次弹出）。
        ::PostMessageW(owner, WM_NULL, 0, 0);
        ::DestroyMenu(menu);
        if (chosen == kMenuRefreshId) {
            action_worker_post_refresh();
        } else if (chosen == kMenuShowWidgetId) {
            action_worker_post_show_widget();
        } else if (chosen >= kMenuScreenBase &&
                   chosen - kMenuScreenBase < entries.size()) {
            // 菜单只改**启用集合**：把所有条目的当前勾复制一份，翻转被点的那一项，整份发给主进程。
            // 不可用但仍勾着的条目会因此被取消；不可用且未勾的条目本来就不在快照里。
            std::size_t const selected = chosen - kMenuScreenBase;
            std::vector<std::wstring> screens;
            for (std::size_t index = 0; index < entries.size(); ++index) {
                bool checked = entries[index].checked;
                if (index == selected) {
                    checked = !checked;
                }
                if (checked) {
                    screens.push_back(entries[index].id);
                }
            }
            action_worker_post_screens(std::move(screens));
        }
        ::DestroyWindow(owner);
    } else {
        log_format(L"cannot create the menu owner window: %s", last_error_text().c_str());
    }

    {
        std::lock_guard<std::mutex> guard(g_menu_mutex);
        g_menu_owner = nullptr;
        g_menu_running = false;
        g_active_menu_entries.clear();
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

// 收菜单：先把本进程的弹出菜单窗口用 ESC 请走，再等**所有**菜单线程句柄。返回 false = 还有
// 线程没退出，调用方必须保留模块（只看“菜单关了”不算收尾证明）。
bool close_menu(unsigned timeout_ms) {
    std::vector<HANDLE> threads;
    HWND owner = nullptr;
    DWORD menu_thread_id = 0;
    {
        std::lock_guard<std::mutex> guard(g_menu_mutex);
        reap_menu_threads_locked();
        threads = g_menu_threads;
        owner = g_menu_owner;
        menu_thread_id = g_menu_thread_id;
    }
    if (threads.empty()) {
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
    unsigned long long const deadline = ::GetTickCount64() + timeout_ms;
    for (HANDLE const thread : threads) {
        unsigned long long const now = ::GetTickCount64();
        unsigned long const remaining =
            now >= deadline ? 0ul : static_cast<unsigned long>(deadline - now);
        if (::WaitForSingleObject(thread, remaining) != WAIT_OBJECT_0) {
            log_format(L"the menu thread did not exit within %u ms", timeout_ms);
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> guard(g_menu_mutex);
        reap_menu_threads_locked();
        // 收尾期间用户又开了一次菜单：句柄还在，不能把“正在开着”抹掉。
        if (g_menu_threads.empty()) {
            g_menu_running = false;
        }
    }
    return true;
}

// ---------- 交互 ----------

void on_pointer_moved(wuxc::Border const& block, wuxi::PointerRoutedEventArgs const& args) {
    if (g_state.tooltip == nullptr || g_state.hover_cells.empty()) {
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
        std::wstring const tip =
            hit >= 0 ? g_state.hover_cells[static_cast<size_t>(hit)].tip : std::wstring();

        // 还是同一条目、还是同一份文字，而且提示框正显示着它：这一次移动什么都不做。**这一条就是
        // "提示框不跟随鼠标"的全部依据**——重写文字、重新定位、重新激活都会让已经弹出来的框跳一下。
        // 内容重建不会清掉这份"已显示身份"，所以读数跨帧刷新（哪怕内容一样）也仍然稳定。
        if (g_state.hover_shown_hit >= 0 && hit == g_state.hover_shown_hit &&
            tip == g_state.hover_shown_tip) {
            return;
        }

        // 只有**命中具体条目**才显示提示。指针在节点之外（条目之间的分隔符、行长之后的空白、
        // 块的边缘）时什么都不显示：这里绝不退回"整块详情"，那会让指针在节点之间移动时忽然
        // 抽出一大块内容并来回闪。本来就没显示的时候也不必再发一次"隐藏"。
        if (hit < 0 || tip.empty()) {
            if (g_state.hover_shown_hit >= 0) {
                hide_tip();
            }
            return;
        }
        // 同一条目自己的 detail 变了：只换文字，位置保持不动。
        if (hit == g_state.hover_shown_hit) {
            update_tip_text(tip);
            return;
        }
        // 命中条目变了（或刚从隐藏状态回到这一条）——这里才重新定位 + 激活。
        POINT cursor{};
        if (::GetCursorPos(&cursor) == FALSE) {
            return;
        }
        cursor.x += 12;
        cursor.y += 20;
        show_tip(hit, tip, cursor);
    } catch (winrt::hresult_error const& failure) {
        log_format(L"hover handling threw %s", hresult_text(failure.code()).c_str());
    } catch (...) {
        log_line(L"hover handling threw an unknown exception");
    }
}

// 进入任务栏自己的回调时，把 thread_local 的“当前目标”设回这块 XAML 所属的那一个。回调可能在
// 负载线程正在做别的目标时被派发，不能靠调用顺序保证。
struct CurrentTargetGuard {
    State* previous = nullptr;
    explicit CurrentTargetGuard(State* target) : previous(g_current) { g_current = target; }
    ~CurrentTargetGuard() { g_current = previous; }
    CurrentTargetGuard(CurrentTargetGuard const&) = delete;
    CurrentTargetGuard& operator=(CurrentTargetGuard const&) = delete;
};

void attach_interaction(wuxc::Border const& block) {
    State* const target = g_current;
    try {
        // 背景必须是**透明但非 null** 的刷子：XAML 的命中测试要求非 null 背景，否则只有字形像素
        // 才吃指针，双击与右键大部分时候收不到。
        block.Background(wuxm::SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0, 0, 0, 0)));
        block.PointerMoved([block, target](winrt::Windows::Foundation::IInspectable const&,
                                           wuxi::PointerRoutedEventArgs const& args) {
            CurrentTargetGuard guard(target);
            on_pointer_moved(block, args);
        });
        block.PointerExited([target](winrt::Windows::Foundation::IInspectable const&,
                                     wuxi::PointerRoutedEventArgs const&) {
            CurrentTargetGuard guard(target);
            hide_tip();
        });
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
    // 托盘那一行比读数高，块必须自己在行里居中：默认的 Stretch 会把它拉满整行、文字顶到上沿。
    g_state.block.VerticalAlignment(wux::VerticalAlignment::Center);
    g_state.label_element = wuxc::TextBlock();
    g_state.label_element.Name(kReadingName);
    g_state.label_element.FontSize(plan.font);
    // 常规字重：前缀与数值只用取色区分。
    g_state.label_element.FontWeight(winrt::Windows::UI::Text::FontWeights::Normal());
    g_state.label_element.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    g_state.label_element.VerticalAlignment(wux::VerticalAlignment::Center);
    g_state.label_element.MaxWidth(max_width);

    std::vector<Segment> const segments = reading_segments(content, colors, plan.per_line);
    apply_segments(g_state.label_element, segments);
    g_state.block.Child(g_state.label_element);
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
        g_state.label_element = nullptr;
        return false;
    }

    try {
        // 刻意**不**调 UpdateLayout：XAML 的布局是异步的，催布局等于在任务栏自己的布局过程里插
        // 一脚。悬停命中用的是离树测量出来的宽度，不依赖这里的实际尺寸；结算后的值由下一轮日志给出。
        (void)0;
    } catch (winrt::hresult_error const&) {
    }
    rebuild_hover_model(content, plan, plan.width, plan.height);
    g_state.block_present.store(true);
    g_any_block_present.store(true);
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
    g_state.label_element = nullptr;
    g_state.column_owner = nullptr;
    g_state.column = nullptr;
    g_state.shifted.clear();
    g_state.runs.clear();
    g_state.applied.clear();
    g_state.hover_cells.clear();
    g_state.block_present.store(false);
    refresh_any_block_present();
    hide_tip();
}

bool update_block(deskfeed::ReadingContent const& content, Colors const& colors, double max_width,
                  std::wstring& error) {
    if (g_state.block == nullptr || g_state.label_element == nullptr) {
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
        if (g_state.label_element.FontSize() != plan.font) {
            g_state.label_element.FontSize(plan.font);
        }
    } else {
        apply_segments(g_state.label_element, segments);
        g_state.label_element.FontSize(plan.font);
        g_state.label_element.MaxWidth(max_width);
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

// ---------- 屏幕 → 任务栏目标 ----------

// 每块任务栏的定位锚点：`Start` 子窗口（Windows 11 的“开始”按钮兼容窗口）左边相对任务栏
// 左边的物理像素偏移。主任务栏与副屏任务栏的左侧内容不同，这个偏移在真机上能区分它们；
// 没有这个子窗口的任务栏记 LONG_MIN，不参与匹配。
constexpr LONG kAnchorNone = LONG_MIN;
constexpr LONG kAnchorTolerancePx = 32;

struct TaskbarAnchor {
    HWND taskbar = nullptr;
    std::wstring screen_id;
    LONG start_offset = kAnchorNone;
};

std::vector<TaskbarAnchor> enumerate_taskbar_anchors() {
    std::vector<TaskbarAnchor> anchors;
    for (TaskbarWindowInfo const& window : enumerate_taskbar_windows()) {
        TaskbarAnchor anchor;
        anchor.taskbar = window.window;
        anchor.screen_id = window.screen_id;
        RECT taskbar_rect{};
        RECT start_rect{};
        HWND const start = ::FindWindowExW(window.window, nullptr, L"Start", nullptr);
        if (start != nullptr && ::GetWindowRect(window.window, &taskbar_rect) != FALSE &&
            ::GetWindowRect(start, &start_rect) != FALSE) {
            anchor.start_offset = start_rect.left - taskbar_rect.left;
        }
        anchors.push_back(std::move(anchor));
    }
    return anchors;
}

// 从托盘元素反查它所在显示器的稳定标识。
//
// XAML 没有公开 API 能把一个 XamlRoot 映射回承载它的 HWND（真机上任务栏元素的 UIA peer 全部为空，
// 拿不到边界矩形；`DesktopWindowXamlSource` 也不从内容侧暴露窗口）。可用的是两侧都存在的几何锚点：
// 每块任务栏的“开始”按钮在岛内的偏移（XAML 侧，DIP × RasterizationScale）与 Win32 `Start`
// 子窗口在任务栏内的偏移（物理像素）是同一个位置。两者对上的那块任务栏就是这块岛的宿主。
//
// 匹配必须唯一：偏移对不上、或同时对上一块以上任务栏时返回空串，调用方按“未识别”处理，
// 绝不按走查顺序猜。
std::wstring screen_id_for_element(wux::FrameworkElement const& element) {
    if (element == nullptr) {
        return std::wstring();
    }
    wux::XamlRoot root{nullptr};
    wux::DependencyObject scope{nullptr};
    try {
        root = element.XamlRoot();
        if (root != nullptr) {
            scope = root.Content();
        }
    } catch (...) {
        return std::wstring();
    }
    if (root == nullptr || scope == nullptr) {
        log_format(L"screen map: this tray element has no loaded XamlRoot");
        return std::wstring();
    }

    wuxc::Control control = scope.try_as<wuxc::Control>();
    if (control == nullptr) {
        try {
            auto const first = wuxi::FocusManager::FindFirstFocusableElement(scope);
            control = first != nullptr ? first.try_as<wuxc::Control>() : nullptr;
        } catch (...) {
        }
    }
    if (control == nullptr) {
        log_format(L"screen map: this island has no first focusable control to anchor on");
        return std::wstring();
    }

    double const scale = root.RasterizationScale() > 0.0 ? root.RasterizationScale() : 1.0;
    LONG offset_x = kAnchorNone;
    try {
        auto const point = control.TransformToVisual(nullptr).TransformPoint({0.0f, 0.0f});
        offset_x = static_cast<LONG>(std::lround(point.X * scale));
    } catch (...) {
    }
    if (offset_x == kAnchorNone) {
        log_format(L"screen map: the anchor control has no island-local position");
        return std::wstring();
    }

    std::wstring id;
    int matches = 0;
    for (TaskbarAnchor const& anchor : enumerate_taskbar_anchors()) {
        if (anchor.start_offset == kAnchorNone) {
            continue;
        }
        if (std::labs(anchor.start_offset - offset_x) <= kAnchorTolerancePx) {
            ++matches;
            id = anchor.screen_id;
            log_format(L"screen map: anchor %ld px matched taskbar %p (%s)", offset_x, anchor.taskbar,
                       anchor.screen_id.c_str());
        }
    }
    if (matches != 1) {
        log_format(L"screen map: anchor %ld px matched %d taskbar(s); leaving this island unidentified",
                   offset_x, matches);
        return std::wstring();
    }
    return id;
}

bool screen_config_contains(std::vector<std::wstring> const& ids, std::wstring const& id) {
    for (std::wstring const& existing : ids) {
        if (taskbar_screen_ids_equal(existing, id)) {
            return true;
        }
    }
    return false;
}

struct ScreenConfigSnapshot {
    bool configured = false;
    std::vector<std::wstring> enabled;
};

ScreenConfigSnapshot snapshot_screen_config() {
    std::lock_guard<std::mutex> guard(g_screen_config_mutex);
    ScreenConfigSnapshot snapshot;
    snapshot.configured = g_screens_configured;
    snapshot.enabled = g_enabled_screens;
    return snapshot;
}

bool same_reading(deskfeed::ReadingContent const& left, deskfeed::ReadingContent const& right) {
    if (left.failure != right.failure || left.alarm != right.alarm || left.reason != right.reason ||
        left.rows.size() != right.rows.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.rows.size(); ++index) {
        deskfeed::ReadingRow const& a = left.rows[index];
        deskfeed::ReadingRow const& b = right.rows[index];
        if (a.label != b.label || a.value != b.value || a.tone != b.tone || a.state != b.state ||
            a.tip != b.tip || a.detail != b.detail) {
            return false;
        }
    }
    return true;
}

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
bool create_probe_target(std::wstring& error) {
    if (!hook_post_probe_armed()) {
        return false;
    }
    HWND const window = g_probe_tray_override.load(std::memory_order_acquire);
    if (window == nullptr) {
        error = L"自测宿主已经登记过托盘目标，当前没有可用的覆盖窗口";
        return false;
    }
    if (::IsWindow(window) == FALSE) {
        error = L"自测宿主指定的托盘窗口已经不在了";
        return false;
    }
    DWORD owner_pid = 0;
    DWORD const owner_thread = ::GetWindowThreadProcessId(window, &owner_pid);
    if (owner_pid != ::GetCurrentProcessId() || owner_thread == 0) {
        error = L"自测宿主指定的托盘窗口不属于本进程";
        return false;
    }
    auto target = std::make_unique<State>();
    target->tray = window;
    target->thread_id = owner_thread;
    target->screen_id = primary_screen_id();
    if (target->screen_id.empty()) {
        target->screen_id = L"probe-screen";
    }
    target->label = L"屏幕 1（主）";
    target->primary = true;
    target->host_window = g_probe_host_window;
    target->slow_apply_ms = g_probe_slow_apply_ms;
    target->probe_placeholder = g_probe_placeholder;
    target->probe_inline = g_probe_inline_mode;
    g_probe_override_mode = true;
    g_targets.push_back(std::move(target));
    return true;
}
#endif

bool create_targets(std::wstring& error) {
    g_targets.clear();
#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
    if (hook_post_probe_armed()) {
        return create_probe_target(error);
    }
#endif
    std::vector<TaskbarWindowInfo> const windows = enumerate_taskbar_windows();
    if (windows.empty()) {
        error = L"找不到任务栏窗口（Shell_TrayWnd / Shell_SecondaryTrayWnd）";
        return false;
    }
    // 所有任务栏窗口必须属于同一条 UI 线程：本模块的钩子收尾证明（完成事件 + 对 T 的 ack ping）
    // 是一条线程一份的，跨线程目标会让“最后一条已退休”冒充所有线程都退休。Windows 11 的任务栏
    // 由同一条 shell UI 线程创建；遇到别的形状就保守地只认第一条线程上的窗口。
    DWORD const taskbar_thread = windows.front().thread_id;
    for (TaskbarWindowInfo const& window : windows) {
        DWORD owner_pid = 0;
        DWORD const owner_thread = ::GetWindowThreadProcessId(window.window, &owner_pid);
        if (owner_pid != ::GetCurrentProcessId()) {
            continue;
        }
        if (owner_thread != taskbar_thread) {
            log_format(L"taskbar screen %s is on thread %lu, not the taskbar thread %lu; skipped",
                       window.screen_id.c_str(), owner_thread, taskbar_thread);
            continue;
        }
        auto target = std::make_unique<State>();
        target->tray = window.window;
        target->thread_id = owner_thread;
        target->screen_id = window.screen_id;
        target->label = window.label;
        target->primary = window.primary;
        target->host_window = g_probe_host_window;
        g_targets.push_back(std::move(target));
    }
    if (g_targets.empty()) {
        error = L"任务栏窗口不属于本进程或没有共同的 UI 线程";
        return false;
    }
    return true;
}

bool execute_hook_action(void* context, HookAction action, std::wstring& error);

// 在任务栏线程上执行：把已经走查到的句柄解析成对象，并映射到屏幕。`xaml_tap_resolve` 返回的
// 元素属于这条线程；屏幕归属的几何锚点也必须在同一条线程上取（XAML 元素有线程亲和性）。
bool resolve_watched_elements(std::wstring& error) {
    std::vector<XamlTapSite> const sites = xaml_tap_sites();
    if (sites.empty()) {
        error = L"没有 XAML 核心交出诊断站点";
        return false;
    }

    struct Pending {
        wux::FrameworkElement element{nullptr};
        DWORD thread_id = 0;
    };
    std::vector<Pending> pending;

    for (std::size_t index = 0; index < sites.size() && index < g_watched_handles.size(); ++index) {
        for (std::uint64_t const handle : g_watched_handles[index]) {
            winrt::Windows::Foundation::IInspectable element{nullptr};
            std::wstring resolve_error;
            if (!xaml_tap_resolve(index, handle, element, resolve_error)) {
                log_format(L"taskbar xaml: core %zu handle %llu: %s", index, handle,
                           resolve_error.c_str());
                continue;
            }
            auto const framework = element.try_as<wux::FrameworkElement>();
            if (framework == nullptr) {
                continue;
            }
#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
            if (g_probe_override_mode) {
                for (auto& target : g_targets) {
                    if (target->tray_element == nullptr) {
                        target->tray_element = framework;
                        target->tray_element_held = true;
                        target->available = true;
                        target->site_index = index;
                        break;
                    }
                }
                continue;
            }
#endif
            std::wstring const screen_id = screen_id_for_element(framework);
            State* chosen = nullptr;
            for (auto& target : g_targets) {
                if (target->tray_element == nullptr && target->thread_id == sites[index].thread_id &&
                    !screen_id.empty() && taskbar_screen_ids_equal(target->screen_id, screen_id)) {
                    chosen = target.get();
                    break;
                }
            }
            if (chosen == nullptr) {
                pending.push_back(Pending{framework, sites[index].thread_id});
                continue;
            }
            chosen->tray_element = framework;
            chosen->tray_element_held = true;
            chosen->available = true;
            chosen->site_index = index;
            log_format(L"taskbar xaml: mapped tray element of core %zu to %s (%s)", index,
                       chosen->label.c_str(), chosen->screen_id.c_str());
        }
    }

    // 探针没认出屏幕时只做**无歧义**的补齐：同一条任务栏线程上只剩一个未配对元素、一个未配对
    // 屏幕时，两者必然是一对。多于一个候选就绝不按走查顺序猜——真机上走查顺序与屏幕顺序不一致，
    // 猜错会把块画到用户没启用的屏幕上。
    if (!pending.empty() && pending.size() != 1) {
        log_format(L"taskbar xaml: %zu tray element(s) could not be tied to a screen; leaving them unmatched",
                   pending.size());
    } else if (pending.size() == 1) {
        State* only_target = nullptr;
        int candidates = 0;
        for (auto& target : g_targets) {
            if (target->tray_element == nullptr && target->thread_id == pending.front().thread_id) {
                only_target = target.get();
                ++candidates;
            }
        }
        if (candidates == 1) {
            only_target->tray_element = pending.front().element;
            only_target->tray_element_held = true;
            only_target->available = true;
            log_format(L"taskbar xaml: %s was the only unmatched screen on this thread",
                       only_target->label.c_str());
        } else {
            log_format(L"taskbar xaml: a tray element could not be tied to a screen and %d candidate(s) remain; leaving them unmatched",
                       candidates);
        }
    }

    bool any = false;
    for (auto const& target : g_targets) {
        if (target->available && target->tray_element != nullptr) {
            any = true;
            log_format(L"taskbar target: %s (%s) on thread %lu", target->label.c_str(),
                       target->screen_id.c_str(), target->thread_id);
        } else {
            log_format(L"taskbar target unavailable: %s (%s)", target->label.c_str(),
                       target->screen_id.c_str());
        }
    }
    if (!any) {
        error = L"没有解析出任何托盘元素";
    }
    return any;
}

// 负载线程：对每个诊断核心走查一次，把句柄交给任务栏线程解析。走查与解析必须分开——
// 走查不能在该核心自己的线程上调用，而解析必须在它自己的线程上。
bool watch_and_resolve_target_elements(std::wstring& error) {
    if (g_diagnostics_closed.load()) {
        error = L"诊断连接已经关掉，无法再解析元素";
        return false;
    }
    if (g_targets.empty()) {
        error = L"没有任务栏目标";
        return false;
    }
    std::vector<XamlTapSite> const sites = xaml_tap_sites();
    if (sites.empty()) {
        error = L"没有 XAML 核心交出诊断站点";
        return false;
    }

    // 只走查与任务栏窗口同一条线程的核心：XAML 岛由任务栏 UI 线程承载，别的核心（开始菜单等）
    // 不可能有这次要的那棵树。少走一个核心就少一次最高风险的操作。
    DWORD const taskbar_thread = g_targets.front()->thread_id;
    std::vector<std::size_t> candidates;
    for (std::size_t index = 0; index < sites.size(); ++index) {
        if (sites[index].thread_id == taskbar_thread) {
            candidates.push_back(index);
        }
    }
    if (candidates.empty()) {
        error = L"没有与任务栏线程同线程的 XAML 核心";
        return false;
    }

    g_watched_handles.assign(sites.size(), {});
    for (std::size_t const index : candidates) {
        std::vector<std::uint64_t> matches;
        std::wstring watch_error;
        if (!xaml_tap_watch(index, kTrayElementName, matches, watch_error)) {
            log_format(L"taskbar xaml: core %zu: %s", index, watch_error.c_str());
            xaml_tap_release_walk(index);
            continue;
        }
        g_watched_handles[index] = std::move(matches);

        hook_post_set_executor(&execute_hook_action, nullptr);
        HookPostResult const posted =
            hook_post_run(g_targets.front()->thread_id, HookAction::Resolve, kPostTimeoutMs);
        // 解析返回的对象各自持有一份引用；这个站点上报过的句柄现在可以整体还回去了。
        g_watched_handles[index].clear();
        xaml_tap_release_walk(index);
        if (!posted.ok) {
            error = posted.error.empty() ? L"任务栏线程没有解析托盘元素" : posted.error;
            continue;
        }

        bool all_assigned = true;
        for (auto const& target : g_targets) {
            if (target->tray_element == nullptr) {
                all_assigned = false;
                break;
            }
        }
        if (all_assigned) {
            break;
        }
    }
    g_watched_handles.clear();

    bool any = false;
    for (auto const& target : g_targets) {
        if (target->available && target->tray_element != nullptr) {
            any = true;
            break;
        }
    }
    if (!any) {
        error = L"没有解析出任何托盘元素";
    }
    return any;
}

void update_menu_model(ScreenConfigSnapshot const& config) {
    std::vector<MenuScreenEntry> entries;
    for (auto const& target : g_targets) {
        if (!target->available || target->tray_element == nullptr) {
            continue;
        }
        MenuScreenEntry entry;
        entry.id = target->screen_id;
        entry.label = target->label.empty() ? std::wstring(L"屏幕") : target->label;
        entry.checked = config.configured ? screen_config_contains(config.enabled, target->screen_id)
                                          : target->primary;
        entries.push_back(std::move(entry));
    }
    // 配置里启用但当前不可用的屏幕也要能在菜单里取消，否则一块拔掉的屏幕会让退化状态永远跟着
    // 用户；它们显示成“已断开的屏幕”，勾仍然是按配置来的。
    for (std::wstring const& id : config.enabled) {
        bool known = false;
        for (MenuScreenEntry const& entry : entries) {
            if (taskbar_screen_ids_equal(entry.id, id)) {
                known = true;
                break;
            }
        }
        if (!known) {
            MenuScreenEntry entry;
            entry.id = id;
            entry.label = L"已断开的屏幕";
            entry.checked = true;
            entries.push_back(std::move(entry));
        }
    }
    std::lock_guard<std::mutex> guard(g_menu_model_mutex);
    g_menu_model = std::move(entries);
}

// 任务栏线程上的动作体：应用当前读数。元素在 attach 阶段已经解析并持有；运行期绝不重连诊断。
bool apply_task(bool allow_placeholder, bool& visible, std::wstring& error) {
    if (g_state.slow_apply_ms != 0) {
        ::Sleep(g_state.slow_apply_ms);
    }
    if (g_state.tray_element == nullptr) {
        error = L"这块屏幕没有可用的托盘元素";
        return false;
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
        // 没有可显示的条目：把块摘掉（整块隐藏），元素照旧持有；读数回来时由 `request_update` 补插。
        if (g_state.block != nullptr) {
            remove_block();
            destroy_tooltip();
        }
        visible = false;
        return true;
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

// 只摘块与悬停提示，保留托盘元素（屏幕配置切换后可能还要插回去）。
bool remove_block_only() {
    remove_block();
    destroy_tooltip();
    return true;
}

bool remove_task(bool& faulted) {
    (void)faulted;
    remove_block_only();
    // 摘除是收尾动作，之后不会再插回去：把持有的托盘元素引用也在这里放掉（元素属于任务栏自己的
    // 树，任务栏自己还持有它，这里放的只是本模块那一份）。
    g_state.tray_element = nullptr;
    g_state.tray_element_held = false;
    g_state.available = false;
    g_state.enabled = false;
    return true;
}

// hook_post 的执行体：**没有异常可以穿出去**（hook_post 里有 SEH + C++ 双边界）。
bool execute_hook_action(void* context, HookAction action, std::wstring& error) {
    if (action == HookAction::Resolve) {
        return resolve_watched_elements(error);
    }
    State* target = static_cast<State*>(context);
    if (target == nullptr && g_targets.size() == 1) {
        // 自测宿主的 DeskFeedProbePostApply 直接调 hook_post_run，没有传 context：只有一块目标时
        // 它没有歧义。
        target = g_targets.front().get();
    }
    if (target == nullptr) {
        error = L"没有可操作的任务栏目标";
        return false;
    }
    CurrentTargetGuard guard(target);
    bool visible = false;
    if (action == HookAction::Apply) {
        return apply_task(target->probe_placeholder, visible, error);
    }
    if (action == HookAction::RemoveBlock) {
        return remove_block_only();
    }
    bool faulted = false;
    return remove_task(faulted);
}

bool post_action(State& target, HookAction action, unsigned timeout_ms, std::wstring& error) {
    if (target.tray == nullptr || ::IsWindow(target.tray) == FALSE) {
        error = L"任务栏窗口已经不在了";
        return false;
    }
    hook_post_set_executor(&execute_hook_action, &target);
    HookPostResult const posted = hook_post_run(target.thread_id, action, timeout_ms);
    if (!posted.ok) {
        error = posted.error.empty() ? L"任务栏线程没有完成动作" : posted.error;
        return false;
    }
    return true;
}

// 把“配置 → 实际显示在哪几块屏”算出来并落到每块任务栏。它是唯一会增删块的入口：
// attach 第一轮、每一帧读数、以及配置变化带来的下一帧都会走这里。
void reconcile_targets(bool force_apply) {
    if (g_targets.empty()) {
        return;
    }

    ScreenConfigSnapshot const config = snapshot_screen_config();

    for (auto& target : g_targets) {
        if (!target->available) {
            continue;
        }
        if (target->tray == nullptr || ::IsWindow(target->tray) == FALSE) {
            log_format(L"taskbar screen %s disappeared; its target is no longer available",
                       target->screen_id.c_str());
            target->available = false;
            target->enabled = false;
            target->block_present.store(false);
            target->tooltip = nullptr;  // 窗口已经没了，不在负载线程碰它的 XAML/子窗口
            refresh_any_block_present();
        }
    }

    std::vector<std::wstring> available;
    for (auto const& target : g_targets) {
        if (target->available && target->tray_element != nullptr) {
            available.push_back(target->screen_id);
        }
    }

    deskfeed::TaskbarScreenSelectionInput input;
    input.configured = config.configured;
    input.enabled = config.enabled;
    input.primary = primary_screen_id();
    input.available = available;
    deskfeed::TaskbarScreenSelection const selection = deskfeed::select_taskbar_screens(input);
    if (selection.degraded != g_last_degraded) {
        g_last_degraded = selection.degraded;
        if (selection.degraded) {
            log_line(L"taskbar: none of the configured screens is available; degrading to the primary "
                     L"screen for this run (the configuration is unchanged)");
        } else {
            log_line(L"taskbar: the configured screen set is available again; degradation is over");
        }
    }

    update_menu_model(config);

    deskfeed::ReadingContent content;
    bool const have_content = current_content(content);
    bool const content_changed =
        force_apply || !g_have_applied_content || !have_content ||
        !same_reading(content, g_last_applied_content);
    bool all_ok = true;

    for (auto& target : g_targets) {
        State& state = *target;
        if (!state.available || state.tray_element == nullptr) {
            state.enabled = false;
            continue;
        }
        bool const wanted = screen_config_contains(selection.selected, state.screen_id);
        // 读数合法但没有条目时，块本就该隐藏；元素继续持有，等下一帧再插回去。
        bool const should_show = wanted && have_content && !content.rows.empty();

        if (!should_show) {
            if (state.block_present.load() || state.tooltip != nullptr) {
                std::wstring error;
                bool ok = true;
                if (state.probe_inline) {
                    CurrentTargetGuard guard(&state);
                    ok = remove_block_only();
                } else {
                    ok = post_action(state, HookAction::RemoveBlock, kPostTimeoutMs, error);
                }
                if (!ok) {
                    all_ok = false;
                    log_format(L"taskbar: cannot hide the block on %s: %s", state.label.c_str(),
                               error.c_str());
                }
            }
            state.enabled = wanted;
            continue;
        }

        if (!state.enabled || force_apply || content_changed || !state.block_present.load()) {
            std::wstring error;
            bool ok = true;
            if (state.probe_inline) {
                CurrentTargetGuard guard(&state);
                bool visible = false;
                ok = apply_task(state.probe_placeholder, visible, error);
            } else {
                ok = post_action(state, HookAction::Apply, kPostTimeoutMs, error);
            }
            if (ok) {
                state.enabled = true;
            } else {
                all_ok = false;
                state.enabled = false;
                log_format(L"taskbar: cannot apply the block on %s: %s", state.label.c_str(),
                           error.c_str());
            }
        }
    }

    if (all_ok) {
        g_last_applied_content = content;
        g_have_applied_content = true;
    }
}

}  // namespace


bool taskbar_xaml_watch(std::wstring& error) {
    if (xaml_tap_sites().empty()) {
        error = L"没有 XAML 核心交出诊断站点";
        return false;
    }

    std::vector<XamlTapSite> const sites = xaml_tap_sites();

    // 自测宿主的结构模式没有真实任务栏窗口：先走查 0 号核心，再用它报出的线程建一个合成目标。
    // 这不会回退到真实 Shell_TrayWnd——探针的树由宿主自己的 XAML 岛提供。
    if (g_targets.empty()) {
#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
        if (hook_post_probe_armed()) {
            if (!create_targets(error)) {
                return false;
            }
        } else
#endif
        {
            std::vector<std::uint64_t> matches;
            if (!xaml_tap_watch(0, kTrayElementName, matches, error)) {
                return false;
            }
            auto target = std::make_unique<State>();
            target->thread_id = sites[0].thread_id;
            target->screen_id = primary_screen_id();
            if (target->screen_id.empty()) {
                target->screen_id = L"probe-screen";
            }
            target->label = L"屏幕 1（主）";
            target->primary = true;
            target->probe_inline = true;
            target->host_window = g_probe_host_window;
            target->site_index = 0;
            target->watched_handles = std::move(matches);
            g_targets.push_back(std::move(target));
            return true;
        }
    }

    // 自测宿主：只挑与目标任务栏同一条线程的核心。额外核心的树用不同的名字（见 probe 的
    // --cores），所以这里不会把别的东西当成托盘。**不重试**：重试就是又一次走查。
    DWORD const target_thread = g_targets.front()->thread_id;
    std::size_t chosen = 0;
    for (std::size_t index = 0; index < sites.size(); ++index) {
        if (sites[index].thread_id == target_thread) {
            chosen = index;
            break;
        }
    }
    std::vector<std::uint64_t> matches;
    if (!xaml_tap_watch(chosen, kTrayElementName, matches, error)) {
        return false;
    }
    g_targets.front()->site_index = chosen;
    g_targets.front()->watched_handles = std::move(matches);
    return true;
}

bool taskbar_xaml_attach(std::wstring const& scope, std::wstring& error) {
    (void)scope;
    hook_post_set_executor(&execute_hook_action, nullptr);

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

    auto restore_iat = [] {
        std::wstring restore_error;
        if (!reg_hook_restore(restore_error)) {
            log_format(L"reg hook: restore failed: %s", restore_error.c_str());
        }
    };

    if (!create_targets(error)) {
        restore_iat();
        return false;
    }

    if (!xaml_tap_start(error)) {
        // 连接失败也要把导入表恢复回去：规避钩子没有用了，不能留在进程里。
        restore_iat();
        return false;
    }

    // 等到本轮应答过的端口都把站点交出来再走查：首站点一到就往下走是过早的，其余核心的
    // SetSite 常常还在路上；走查一开始、诊断关闭一跑，它们就会在关闭标志之后到达并被拒收，
    // 端口取走的那份模块引用就留在 loader 账上（回到注入器手里就是"引用不是 1"）。
    // 期限仍是原来的 kSiteAttempts * kSiteDelayMs；到限就用现有站点如实继续，不按端口数补引用。
    std::size_t const sites_ready = xaml_tap_wait_for_sites(kSiteAttempts * kSiteDelayMs);
    log_format(L"diagnostics ready: %zu site(s) attached before the walk", sites_ready);
    // 连接过程里 XAML 可能又载入了模块，补扫一次导入表。
    reg_hook_rescan();

    bool const resolved = watch_and_resolve_target_elements(error);

    // 解析阶段到此结束：诊断这条线整条关掉（成功、失败、没走成都关），并且**立刻**恢复导入表——
    // 那层规避只需要覆盖走查这一段。
    std::wstring tap_detail;
    if (!xaml_tap_stop(2000, tap_detail)) {
        log_format(L"diagnostics teardown is incomplete: %s", tap_detail.c_str());
    }
    g_diagnostics_closed.store(true);
    restore_iat();

    if (!resolved) {
        return false;
    }

    // 第一轮：按配置/缺省把块插到命中的屏幕上；退化只影响本轮落点，不改配置。
    reconcile_targets(true);

    // 成功的判据是"走查成功 + 至少持有一个托盘元素"，**不是**"此刻树上有块"：首次读数是合法的
    // Waiting/[] 时块本来就不该在（`xaml_block_present` 独立表达这件事），把"当前没有块"当成 attach
    // 失败会让负载随后丢掉每一个正常快照。
    bool any = false;
    for (auto const& target : g_targets) {
        if (target->available && target->tray_element != nullptr) {
            any = true;
            break;
        }
    }
    g_attached.store(any);
    return any;
}

bool taskbar_xaml_request_update(deskfeed::ReadingContent const& content) {
    {
        std::lock_guard<std::mutex> guard(g_content_mutex);
        g_content = content;
        g_have_content = true;
    }
    if (g_targets.empty()) {
        return false;
    }
    // 运行期绝不走查，也绝不重连诊断：诊断连接在解析阶段就已经关掉。这里只根据已持有的元素
    // 增删块、就地换字。
    reconcile_targets(false);
    bool any = false;
    for (auto const& target : g_targets) {
        if (target->available && target->enabled) {
            any = true;
            break;
        }
    }
    return any;
}

void taskbar_xaml_set_screen_config(bool configured, std::vector<std::wstring> const& screens) {
    std::lock_guard<std::mutex> guard(g_screen_config_mutex);
    g_screens_configured = configured;
    g_enabled_screens = screens;
}

bool taskbar_xaml_detach(unsigned timeout_ms, std::wstring& detail) {
    bool safe = true;

    // 第一步收菜单：菜单是负载自己线程上的模态 TrackPopupMenu，还开着就拆除会留下"块没了、菜单
    // 还在"。线程没退出就是没退休。
    if (!close_menu(timeout_ms)) {
        safe = false;
        detail = L"菜单线程没有在期限内退出";
    }

    // 第二步：每块任务栏上摘块并销毁悬停窗口。所有目标共享同一条任务栏线程（create_targets 已经
    // 拒绝跨线程的形状），所以这里按顺序投递，最后只做一次钩子静止证明。
    for (auto& target : g_targets) {
        State& state = *target;
        bool const need_remove = state.block_present.load() || state.tooltip != nullptr;
        if (!need_remove) {
            continue;
        }
        bool ok = true;
        if (state.probe_inline) {
            // 自测宿主：调用线程就是树所属的那条线程，直接做（它可能没有任务栏窗口）。
            CurrentTargetGuard guard(&state);
            bool faulted = false;
            ok = remove_task(faulted);
        } else if (state.tray == nullptr || ::IsWindow(state.tray) == FALSE) {
            // 窗口已经没了：XAML 树随窗口消失，不能再投递。它的块不可能还可见。
            state.block_present.store(false);
            state.tooltip = nullptr;
            refresh_any_block_present();
            continue;
        } else {
            std::wstring error;
            ok = post_action(state, HookAction::Remove, timeout_ms, error);
            if (!ok && !detail.empty()) {
                detail += L"; ";
            }
            if (!ok) {
                detail += L"任务栏线程没有摘掉 " + state.label + L" 的读数块";
            }
        }
        if (!ok) {
            safe = false;
        }
    }

    // 钩子的静止证明（完成事件 + 对 T 的窗口做一次非排队 ping），拿到才归还自引用。
    DWORD ack_thread = 0;
    HWND ack_window = g_ack_window_override;
    for (auto const& target : g_targets) {
        if (target->tray != nullptr && ::IsWindow(target->tray) != FALSE) {
            ack_thread = target->thread_id;
            if (ack_window == nullptr) {
                ack_window = target->tray;
            }
            break;
        }
    }
    std::wstring hook_detail;
    bool const hook_proven = hook_post_retire(ack_window, ack_thread, timeout_ms, hook_detail);
    if (!hook_proven) {
        safe = false;
        if (!detail.empty()) {
            detail += L"; ";
        }
        detail += hook_detail;
    }

    if (hook_proven && safe) {
        g_targets.clear();
        g_any_block_present.store(false);
    }
    g_attached.store(false);
    return safe;
}

bool taskbar_xaml_block_visible() { return g_any_block_present.load(); }

void taskbar_xaml_probe_set_slow_apply(unsigned ms) {
    g_probe_slow_apply_ms = ms;
    for (auto& target : g_targets) {
        target->slow_apply_ms = ms;
    }
}

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
    if (g_targets.empty()) {
        if (!create_targets(error)) {
            return false;
        }
    }
    State& target = *g_targets.front();
    CurrentTargetGuard guard(&target);
    target.probe_placeholder = allow_placeholder;
    target.probe_inline = true;
    target.slow_apply_ms = g_probe_slow_apply_ms;
    target.host_window = g_probe_host_window;
    hook_post_set_executor(&execute_hook_action, &target);

    if (target.tray_element == nullptr && !target.watched_handles.empty()) {
        winrt::Windows::Foundation::IInspectable element{nullptr};
        std::uint64_t const handle = target.watched_handles.front();
        if (!xaml_tap_resolve(target.site_index, handle, element, error)) {
            xaml_tap_release_walk(target.site_index);
            return false;
        }
        target.tray_element = element.try_as<wux::FrameworkElement>();
        target.tray_element_held = target.tray_element != nullptr;
        target.available = target.tray_element != nullptr;
        if (target.tray_element != nullptr) {
            log_format(L"resolved %s", winrt::get_class_name(target.tray_element).c_str());
        }
    }

    if (dump_only) {
        if (target.site_index < xaml_tap_sites().size()) {
            xaml_tap_release_walk(target.site_index);
        }
        return target.tray_element != nullptr;
    }

    if (target.tray_element == nullptr) {
        error = L"还没有解析出托盘元素（自测宿主必须先走查一次）";
        return false;
    }

    bool const ok = apply_task(allow_placeholder, visible, error);
    if (ok && target.site_index < xaml_tap_sites().size()) {
        xaml_tap_release_walk(target.site_index);
    }
    return ok;
}

int taskbar_xaml_probe_hover_cells(long* out, int capacity) {
    if (out == nullptr || capacity < 4 || g_targets.empty()) {
        return 0;
    }
    State& target = *g_targets.front();
    CurrentTargetGuard guard(&target);
    if (target.block == nullptr || !target.hover_origin_known) {
        return 0;
    }
    int written = 0;
    double const scale = static_cast<double>(window_dpi(target.tray)) / 96.0;
    out[written++] = target.hover_origin.x;
    out[written++] = target.hover_origin.y;
    out[written++] = static_cast<long>(target.hover_width * scale);
    out[written++] = static_cast<long>(target.hover_height * scale);
    for (HoverCell const& cell : target.hover_cells) {
        if (written + 4 > capacity) {
            break;
        }
        out[written++] = static_cast<long>(target.hover_origin.x + cell.left * scale);
        out[written++] = static_cast<long>(target.hover_origin.y + cell.top * scale);
        out[written++] = static_cast<long>(target.hover_origin.x + cell.right * scale);
        out[written++] = static_cast<long>(target.hover_origin.y + cell.bottom * scale);
    }
    return written;
}

void taskbar_xaml_set_probe_inline(bool inline_mode) {
    g_probe_inline_mode = inline_mode;
    for (auto& target : g_targets) {
        target->probe_inline = inline_mode;
    }
}

void taskbar_xaml_set_host_window(HWND window) {
    g_probe_host_window = window;
    for (auto& target : g_targets) {
        target->host_window = window;
    }
    // 钩子收尾时那次"帧已弹掉"的 ping 需要一个**属于任务栏线程**的窗口。真实注入里是
    // Shell_TrayWnd；自测宿主没有它，就由宿主交一个自己的、user32 类的窗口（例如 STATIC），
    // 绝不能用本模块自己的窗口过程当确认目标。
    hook_post_set_ack_window(window);
}

#ifdef DESKFEED_PAYLOAD_PROBE_BUILD
void taskbar_xaml_probe_set_tray_override(HWND window) {
    g_probe_tray_override.store(window, std::memory_order_release);
    if (window == nullptr) {
        g_probe_override_mode = false;
        g_targets.clear();
    }
}
#endif  // DESKFEED_PAYLOAD_PROBE_BUILD

}  // namespace deskfeed::payload
