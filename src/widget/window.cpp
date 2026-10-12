#include "widget/window.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
// WIN32_LEAN_AND_MEAN 的 windows.h 不带 objidl.h，而 GDI+ 的头里要用 STDMETHOD / IUnknown：
// 先显式带进来，否则是一串“未定义基类 IUnknown”。
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "core/utf8.h"
#include "display/reading.h"
#include "platform/log.h"
#include "platform/win32_util.h"
#include "widget/fullscreen.h"
#include "widget/text_metrics.h"
#include "widget/theme.h"

// 实现结构说明：window.h 是主进程依赖的稳定接口，里面只声明了 create / apply /
// show_from_request / destroy / handle 与一个不透明的 Impl。为了让实现留在本文件、又不把
// 内部成员塞回公开头，所有实现状态放在下面这个文件内部的 WidgetState 里，自由函数只认它；
// `WidgetWindow::Impl` 只是继承它的一次空包装。这样公开头不用随内部改动而变。
namespace deskfeed::widget {
namespace {

constexpr wchar_t kWindowTitle[] = L"DeskFeed";
constexpr wchar_t kSegmentSeparator[] = L" · ";

constexpr UINT_PTR kTimerTopmost = 1;
constexpr UINT_PTR kTimerSaveState = 2;
constexpr UINT kTopmostGuardMs = 1000;
constexpr UINT kSaveDebounceMs = 1000;
// 沿 GW_HWNDPREV 往上扫的步数上限（兜底判据用）。
constexpr int kTopmostScanLimit = 48;
// 命中测试的采样点从窗口矩形四角往里缩这么多像素：矩形边界上的点会落到相邻窗口上，
// 内缩几个像素才问的是“我们的内容区上面是谁”。
constexpr int kTopmostProbeInset = 3;
// 矩形小到内缩之后取不出四个不同的角时，只取中心一个点。
constexpr int kTopmostProbeMinSize = 2 * kTopmostProbeInset + 4;
// 连读到“被压住、抬不回去”时，那行日志的节流间隔。
constexpr unsigned long long kTopmostCoverLogMs = 10000;
// 默认锚点从主屏可用区右下角往里缩这么多。
constexpr int kEdgeMargin = 10;
constexpr int kDragSlop = 3;
constexpr unsigned long long kDoubleTapMs = 400;
// 同一次右键手势里 WM_RBUTTONUP 与随后的 WM_CONTEXTMENU 之间只隔几毫秒；这个去重窗口比它宽、
// 又远比用户两次真的点菜单短。
constexpr unsigned long long kMenuRepeatMs = 400;
constexpr int kDoubleTapSlop = 8;
// 视觉行最多 kMaxReadingLines 条，这里留一点余量。
constexpr std::size_t kMaxTooltips = 8;
// 提示文本的换行宽度（像素），与任务栏那份悬停提示同一个值。
// **不设它就等于只有一行**：多行 tip 里的 '\n' 会被丢掉、整条挤成一行，而且那条绘制路径画出来
// 的字明显更重。微软的文档也是这一条：多行 tooltip 要用 TTM_SETMAXTIPWIDTH 指定显示宽度。
constexpr int kTipMaxWidth = 420;
// TTM_ADDTOOL 认的 cbSize 看进程里生效的是哪一版 comctl32：v6 认 sizeof(TOOLINFOW)（64 位下 72），
// v5 认不到 72，只认到 hinst 为止的那一版（56）或 TTTOOLINFOW_V2_SIZE（64）。登记时按这个顺序试，
// 把过了的那一个留给后面所有 TTM_* 消息用（它们要求 cbSize 一致）；失败时 GetLastError() 是 0，
// 光看错误码查不出来，只能试。
constexpr UINT kToolInfoCandidates[] = {sizeof(TOOLINFOW), TTTOOLINFOW_V2_SIZE,
                                        offsetof(TOOLINFOW, lParam)};
// 窗口比这还窄就不落位（内容还在一批批排进来时会有很小的中间尺寸）。
constexpr int kMinWidthForPlacement = 60;

// DWMWA_WINDOW_CORNER_PREFERENCE 与它的取值。
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr int kDwmcpRound = 2;

// 右键菜单的命令号。窗口没有子控件，WM_COMMAND 只会来自外部按命令号投递
// （离线夹具就是这么驱动菜单项的）。
enum MenuCommand : UINT {
    kCmdRefresh = 1,
    kCmdExpanded = 2,
    kCmdTaskbarComponent = 3,
    kCmdHide = 4,
    kCmdExit = 5,
    // 这一项排在菜单第二，命令号接在旧的五个后面，不动已有取值。
    kCmdTopmost = 6,
};

// 100 纳秒一格，这里换算成毫秒。双击判定与日志节流用它。
unsigned long long now_ms() {
    FILETIME file_time{};
    ::GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return value.QuadPart / 10000ULL;
}

// GDI+ 是进程级的：用引用计数包一层，窗口建/毁各算一次，最后一个窗口销毁时才关掉它。
int g_gdiplus_users = 0;
ULONG_PTR g_gdiplus_token = 0;
bool g_gdiplus_ready = false;

bool gdiplus_acquire() {
    if (g_gdiplus_users == 0) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, nullptr) != Gdiplus::Ok) {
            g_gdiplus_ready = false;
            return false;
        }
        g_gdiplus_ready = true;
    }
    ++g_gdiplus_users;
    return true;
}

void gdiplus_release() {
    if (g_gdiplus_users == 0) {
        return;
    }
    --g_gdiplus_users;
    if (g_gdiplus_users == 0 && g_gdiplus_ready) {
        Gdiplus::GdiplusShutdown(g_gdiplus_token);
        g_gdiplus_ready = false;
    }
}

std::wstring wide(std::string const& utf8) { return wide_from_utf8(utf8); }

// 全部实现状态。
struct WidgetState {
    WidgetOptions options{};
    WidgetCallbacks callbacks{};
    WindowPreferences preferences{};

    HWND hwnd = nullptr;
    FontSet fonts{};
    TextMetrics metrics{};
    HDC measure_dc = nullptr;
    HBRUSH surface_brush = nullptr;
    HPEN border_pen = nullptr;

    ReadingContent content{};
    std::vector<ReadingLine> lines{};  // 已折行的视觉行
    std::wstring detail_text{};        // 展开详情（按当前宽度排版用）
    ViewKind kind = ViewKind::Waiting;
    bool rendered_once = false;
    bool has_content = false;
    bool ready_to_show = false;
    bool fullscreen = false;
    bool dwm_rounded = false;
    bool dragging = false;
    bool drag_moved = false;
    POINT drag_from{0, 0};
    POINT drag_origin{0, 0};
    // 拖动开始时的偏好：落盘失败要回滚到这一份。
    WindowPreferences drag_before{};
    // 锚点改过、还没落盘：定时器到点时按 anchor_before 回滚。
    bool anchor_dirty = false;
    WindowPreferences anchor_before{};
    unsigned long long last_tap_ms = 0;
    POINT last_tap_point{0, 0};
    unsigned long long last_menu_ms = 0;
    // “被别的窗口压住、补完置顶还压着”那行日志上一次记的时刻（毫秒）。0 = 上一 tick 没被压住，
    // 于是下一次被压住立刻记一行；连续被压住则 10 秒最多一行。
    unsigned long long topmost_cover_log_ms = 0;

    int width = 0;
    int height = 0;
    std::vector<RECT> line_rects{};
    RECT detail_rect{0, 0, 0, 0};

    HWND tip = nullptr;
    std::vector<std::wstring> tooltip_text{};
    std::vector<bool> tooltip_added{};
    std::vector<RECT> tooltip_rects{};
    // 这一版 comctl32 认下的 TTM_* cbSize；0 = 还没定。
    UINT tool_info_size = 0;
    // 上一次记进日志的“挂上提示的行数”，只为让那行日志在行数没变时闭嘴。
    std::size_t registered_tooltip_rows = 0;

    HMENU menu = nullptr;

    // 状态日志去重：内容没变就不写第二行。
    std::wstring last_status{};
    bool gdiplus_held = false;
};

void create_tooltip_of(WidgetState& self);
bool add_tool_of(WidgetState& self, TOOLINFOW& tool);
void update_tooltips_of(WidgetState& self);
void create_context_menu_of(WidgetState& self);
int line_width_of(WidgetState const& self, ReadingLine const& line);
void rebuild_lines_of(WidgetState& self);
void relayout_of(WidgetState& self);
std::wstring content_reason_of(WidgetState const& self);
void update_fullscreen_state_of(WidgetState& self);
void paint_of(WidgetState& self, HDC target);
void paint_border_of(WidgetState& self, HDC dc);
void arm_save_timer_of(WidgetState& self);
void place_by_anchor_of(WidgetState& self);
void remember_anchor_of(WidgetState& self);
POINT clamp_point_of(WidgetState const& self, POINT origin, SIZE size);
POINT default_anchor_of(WidgetState const& self);
void apply_corner_shape_of(WidgetState& self);
bool set_corner_preference_of(WidgetState const& self, int preference);
void apply_topmost_of(WidgetState& self);
bool is_topmost_of(WidgetState const& self);
bool overlapped_from_above_of(WidgetState const& self);
bool covered_by_foreign_window_of(WidgetState const& self, std::wstring* detail);
void verify_topmost_of(WidgetState& self);
bool save_preferences_of(WidgetState& self, WindowPreferences const& before);
void set_user_hidden_of(WidgetState& self, bool hidden);
void set_expanded_of(WidgetState& self, bool expanded);
void record_status_of(WidgetState& self, std::wstring const& state, std::wstring const& reason);
void on_lbutton_down_of(WidgetState& self, POINT screen);
void on_mouse_move_of(WidgetState& self, POINT screen);
void on_lbutton_up_of(WidgetState& self, POINT screen);
void finish_drag_after_move_of(WidgetState& self);
void on_rbutton_up_of(WidgetState& self, POINT screen);
void show_context_menu_of(WidgetState& self);
void run_command_of(WidgetState& self, UINT command);
// 拆窗口/提示/菜单/计时器（**不落盘**），与释放 GDI/GDI+ 资源分开：外部 DestroyWindow 之后
// 窗口已经不在，收尾仍要把资源放掉；建窗失败时也走拆解，但一个字节的偏好都不写。
void teardown_window_of(WidgetState& self);
void release_resources_of(WidgetState& self);
void destroy_window_of(WidgetState& self);

void create_tooltip_of(WidgetState& self) {
    // 通用控件类要先登记：TOOLTIPS_CLASS 是 comctl32 注册的，不保证在 DllMain 之后就绪。
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
    ::InitCommonControlsEx(&controls);

    self.tip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                 WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                                 CW_USEDEFAULT, CW_USEDEFAULT, self.hwnd, nullptr,
                                 self.options.instance, nullptr);
    if (self.tip == nullptr) {
        log_format(L"widget: no tooltip control (%s); hover text will be missing",
                   last_error_text().c_str());
        return;
    }
    // 悬浮窗几乎总是非活动窗口，所以提示要带 TTS_ALWAYSTIP 才能在别处有焦点时弹出来。
    ::SetWindowPos(self.tip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ::SendMessageW(self.tip, TTM_SETDELAYTIME, TTDT_INITIAL, MAKELPARAM(250, 0));
    ::SendMessageW(self.tip, TTM_SETDELAYTIME, TTDT_RESHOW, MAKELPARAM(80, 0));
    ::SendMessageW(self.tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, MAKELPARAM(30000, 0));
    // 换行宽度：折行之后一行的 tip 是多行文本，不设这一条就会被挤成一行。
    ::SendMessageW(self.tip, TTM_SETMAXTIPWIDTH, 0, kTipMaxWidth);

    self.tooltip_text.assign(kMaxTooltips, std::wstring());
    self.tooltip_added.assign(kMaxTooltips, false);
    self.tooltip_rects.assign(kMaxTooltips, RECT{0, 0, 0, 0});
    // 这里不提前登记：行矩形还是空的时候登记进去就是个空的命中区，等第一次
    // update_tooltips 拿到真实的行矩形再按行登记。
}

bool add_tool_of(WidgetState& self, TOOLINFOW& tool) {
    for (UINT const candidate : kToolInfoCandidates) {
        // 已经定下来了就只试那一个：cbSize 一旦被某一版接受，后面所有消息都得用同一个值。
        if (self.tool_info_size != 0 && candidate != self.tool_info_size) {
            continue;
        }
        tool.cbSize = candidate;
        if (::SendMessageW(self.tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)) == FALSE) {
            continue;
        }
        if (self.tool_info_size == 0) {
            self.tool_info_size = candidate;
            // 这一行是“悬停提示用的是哪一版 comctl32、按多宽折行”的唯一证据。
            log_format(L"widget: the tooltip tool is registered (cbSize %u, comctl32 %s, "
                       L"wrap width %d px)",
                       candidate, candidate == sizeof(TOOLINFOW) ? L"v6" : L"v5", kTipMaxWidth);
        }
        return true;
    }
    return false;
}

void update_tooltips_of(WidgetState& self) {
    // 窗口已经不在了（外部 DestroyWindow）时不要向 comctl32 登记一个死句柄。
    if (self.tip == nullptr || self.hwnd == nullptr) {
        return;
    }
    for (std::size_t index = 0; index < self.tooltip_text.size(); ++index) {
        bool const visible = index < self.lines.size() && index < self.line_rects.size();
        // 悬停文本与量具的 `--reading` 走同一个函数，两边不可能对同一条行给出不同的提示。
        std::wstring const text = visible ? wide(reading_line_tip(self.lines[index])) : std::wstring();

        // 隐藏的行把命中矩形收成空的，鼠标扫过去不会冒提示。
        RECT const rect = visible ? self.line_rects[index] : RECT{0, 0, 0, 0};
        bool const has_rect = rect.right > rect.left && rect.bottom > rect.top;

        TOOLINFOW tool{};
        tool.cbSize = self.tool_info_size != 0 ? self.tool_info_size : kToolInfoCandidates[0];
        tool.hwnd = self.hwnd;
        tool.uId = static_cast<UINT>(index);
        tool.rect = has_rect ? rect : RECT{0, 0, 0, 0};

        if (self.tooltip_added[index]) {
            RECT const& known = self.tooltip_rects[index];
            bool const moved = tool.rect.left != known.left || tool.rect.top != known.top ||
                               tool.rect.right != known.right || tool.rect.bottom != known.bottom;
            if (!has_rect || moved) {
                // 行没了，或者命中矩形挪了：先摘掉，下面按需要重新登记。
                // 不用 TTM_NEWTOOLRECT——它的返回值不可信；TTM_DELTOOL 的返回值同样不可信，
                // 但它对工具数量的影响看得见。
                ::SendMessageW(self.tip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
                self.tooltip_added[index] = false;
                self.tooltip_rects[index] = RECT{0, 0, 0, 0};
            } else if (text != self.tooltip_text[index]) {
                self.tooltip_text[index] = text;
                tool.lpszText = self.tooltip_text[index].data();
                ::SendMessageW(self.tip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
            }
        }

        if (!self.tooltip_added[index]) {
            if (!has_rect) {
                continue;  // 这一行现在不该冒提示
            }
            self.tooltip_text[index] = text;
            tool.uFlags = TTF_SUBCLASS;
            tool.lpszText = self.tooltip_text[index].data();
            if (!add_tool_of(self, tool)) {
                log_format(L"widget: TTM_ADDTOOL failed for tool %zu (rect %ld,%ld,%ld,%ld) "
                           L"error=%lu; hover text for this row is missing",
                           index, tool.rect.left, tool.rect.top, tool.rect.right, tool.rect.bottom,
                           ::GetLastError());
                continue;
            }
            self.tooltip_added[index] = true;
            self.tooltip_rects[index] = tool.rect;
        }
    }

    // 登记成功后记一行：这一行是“悬停提示到底挂上了几行”的唯一证据。只在挂上的行数变化时记。
    std::size_t added = 0;
    for (bool const flag : self.tooltip_added) {
        if (flag) {
            ++added;
        }
    }
    if (added != self.registered_tooltip_rows) {
        self.registered_tooltip_rows = added;
        log_format(L"widget: hover tips registered for %zu of %zu row(s) "
                   L"(the control reports %lld tool(s))",
                   added, self.lines.size(),
                   static_cast<long long>(::SendMessageW(self.tip, TTM_GETTOOLCOUNT, 0, 0)));
    }
}

void create_context_menu_of(WidgetState& self) {
    self.menu = ::CreatePopupMenu();
    if (self.menu == nullptr) {
        log_line(L"widget: cannot build the context menu");
        return;
    }
    // 六项：刷新 / 置顶悬浮窗 / 切换详细与精简 / 任务栏组件 / 隐藏与显示悬浮窗 / 退出。
    // 勾只用来表示开关状态（置顶、任务栏组件）；详细与隐藏那两项是动态文案，不表示状态。
    ::AppendMenuW(self.menu, MF_STRING, kCmdRefresh, L"刷新");
    ::AppendMenuW(self.menu, MF_STRING, kCmdTopmost, L"置顶悬浮窗");
    ::AppendMenuW(self.menu, MF_STRING, kCmdExpanded,
                  self.preferences.expanded ? L"切换至精简" : L"切换至详细");
    UINT const component_flags =
        MF_STRING | (self.options.taskbar_available ? 0u : static_cast<UINT>(MF_GRAYED));
    ::AppendMenuW(self.menu, component_flags, kCmdTaskbarComponent, L"任务栏组件");
    // 「隐藏悬浮窗」是开关（`[window] hidden` 跨启动保持），按当前状态给动态文案：
    // 藏着就写「显示悬浮窗」，显示着就写「隐藏悬浮窗」。
    ::AppendMenuW(self.menu, MF_STRING, kCmdHide,
                  self.preferences.hidden ? L"显示悬浮窗" : L"隐藏悬浮窗");
    ::AppendMenuW(self.menu, MF_STRING, kCmdExit, L"退出");
    ::CheckMenuItem(self.menu, kCmdTopmost,
                    MF_BYCOMMAND | (self.preferences.topmost ? MF_CHECKED : MF_UNCHECKED));
    ::CheckMenuItem(self.menu, kCmdTaskbarComponent,
                    MF_BYCOMMAND | (self.preferences.taskbar_component ? MF_CHECKED : MF_UNCHECKED));
}

int line_width_of(WidgetState const& self, ReadingLine const& line) {
    int width = 0;
    bool first = true;
    for (ReadingRow const& segment : line.segments) {
        if (!first) {
            width += tight_width(self.measure_dc, kSegmentSeparator, self.fonts.row_value);
        }
        first = false;
        if (!segment.label.empty()) {
            width += tight_width(self.measure_dc, wide(segment.label), self.fonts.row_prefix) +
                     self.metrics.space_width;
        }
        width += tight_width(self.measure_dc, wide(segment.value), self.fonts.row_value);
    }
    return width;
}

void rebuild_lines_of(WidgetState& self) {
    self.lines = reading_pack(self.content, reading_per_line_for(self.content.rows.size()));
}

void relayout_of(WidgetState& self) {
    self.line_rects.clear();
    self.detail_rect = RECT{0, 0, 0, 0};
    self.detail_text.clear();

    int content_width = 0;
    int y = kPaddingTop;
    for (ReadingLine const& line : self.lines) {
        int const width = line_width_of(self, line);
        self.line_rects.push_back(
            RECT{kPaddingLeft, y, kPaddingLeft + width, y + self.metrics.row_height});
        content_width = std::max(content_width, width);
        y += self.metrics.row_height + kRowGap;
    }

    if (self.preferences.expanded && !self.content.rows.empty()) {
        self.detail_text = wide(reading_detail_text(self.content));
        int const detail_width =
            wrapped_width(self.measure_dc, self.detail_text, self.fonts.detail, kDetailMaxWidth);
        int const detail_height =
            wrapped_height(self.measure_dc, self.detail_text, self.fonts.detail, kDetailMaxWidth);
        y += kDetailGap;
        self.detail_rect =
            RECT{kPaddingLeft, y, kPaddingLeft + std::max(detail_width, 1), y + detail_height};
        content_width = std::max(content_width, detail_width);
        y += detail_height;
    }

    int const width = std::max(content_width + kPaddingLeft + kPaddingRight, 1);
    int const height = std::max(y + kPaddingBottom, 1);
    if (width == self.width && height == self.height) {
        return;
    }
    self.width = width;
    self.height = height;
    ::SetWindowPos(self.hwnd, nullptr, 0, 0, self.width, self.height,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // 尺寸变了：形状先跟上，然后（可见且够宽时）按右下角锚点重新落位。
    apply_corner_shape_of(self);
    if (::IsWindowVisible(self.hwnd) && self.width >= kMinWidthForPlacement) {
        place_by_anchor_of(self);
    }
}

std::wstring content_reason_of(WidgetState const& self) {
    if (self.content.rows.empty()) {
        return L"没有可显示的条目";
    }
    std::wstring const reason = wide(self.content.reason);
    if (self.content.failure) {
        return reason.empty() ? std::wstring(L"取数失败") : std::wstring(L"取数失败：") + reason;
    }
    if (self.content.alarm) {
        // 有值但整体失效：值还在（不丢数据），但状态行必须明说整体失效，而不是只报一个条数。
        return reason.empty() ? std::wstring(L"整体失效") : std::wstring(L"整体失效：") + reason;
    }
    return L"共 " + std::to_wstring(self.content.rows.size()) + L" 项";
}

void update_fullscreen_state_of(WidgetState& self) {
    std::wstring detail;
    FullscreenSignal const signal = fullscreen_foreground(&detail);
    bool const fullscreen = signal != FullscreenSignal::kNone;
    if (fullscreen == self.fullscreen) {
        return;  // 状态没变：不记日志、不动窗口
    }
    self.fullscreen = fullscreen;
    std::wstring const signal_name = fullscreen_signal_name(signal);

    if (fullscreen) {
        // 整窗藏起来。用户隐藏标记一个字都不动：那是用户的意图，全屏不该把它抹掉。
        ::ShowWindow(self.hwnd, SW_HIDE);
        log_format(L"widget: a fullscreen application owns the screen (signal=%s, detail=%s); "
                   L"hiding until it ends",
                   signal_name.c_str(), detail.c_str());
        record_status_of(self, L"隐藏", L"前台是全屏程序（" + detail + L"），悬浮窗让位");
        return;
    }

    // 全屏结束：按“当时最新的意图”恢复，所以重新读一次当前偏好。
    if (!self.ready_to_show || !self.has_content || self.preferences.hidden ||
        self.options.force_hidden) {
        log_format(L"widget: the fullscreen application is gone (signal=%s, detail=%s); staying "
                   L"hidden (content=%d user_hidden=%d)",
                   signal_name.c_str(), detail.c_str(), self.has_content ? 1 : 0,
                   self.preferences.hidden ? 1 : 0);
        record_status_of(self, L"隐藏",
                         L"全屏程序已退出，但悬浮窗仍不显示（内容=" +
                             std::wstring(self.has_content ? L"有" : L"无") + L"，用户隐藏=" +
                             std::wstring(self.preferences.hidden ? L"是" : L"否") + L"）");
        return;
    }

    place_by_anchor_of(self);
    ::ShowWindow(self.hwnd, SW_SHOWNOACTIVATE);
    if (self.preferences.topmost) {
        // 藏起来这段时间里可能被别的窗口盖过：立刻补一次，不等下一秒那次 tick。
        apply_topmost_of(self);
    }
    log_format(L"widget: the fullscreen application is gone (signal=%s, detail=%s); showing again",
               signal_name.c_str(), detail.c_str());
    record_status_of(self, L"显示", content_reason_of(self) + L"（全屏程序已退出，恢复显示）");
}

void paint_of(WidgetState& self, HDC target) {
    RECT const client{0, 0, self.width, self.height};
    HDC const buffer = ::CreateCompatibleDC(target);
    HBITMAP const bitmap =
        buffer != nullptr ? ::CreateCompatibleBitmap(target, self.width, self.height) : nullptr;
    HGDIOBJ previous_bitmap = nullptr;
    HDC dc = target;
    if (buffer != nullptr && bitmap != nullptr) {
        previous_bitmap = ::SelectObject(buffer, bitmap);
        dc = buffer;
    }

    ::FillRect(dc, &client, self.surface_brush);
    ::SetBkMode(dc, TRANSPARENT);

    for (std::size_t index = 0; index < self.lines.size() && index < self.line_rects.size(); ++index) {
        ReadingLine const& line = self.lines[index];
        RECT const rect = self.line_rects[index];
        int x = rect.left;
        bool first = true;
        for (ReadingRow const& segment : line.segments) {
            if (!first) {
                // 段与段之间的分隔符用弱色，与任务栏那块“前缀弱色、数值主色”的分工一致。
                HGDIOBJ const previous = ::SelectObject(dc, self.fonts.row_value);
                ::SetTextColor(dc, kTertiary);
                RECT separator_rect{x, rect.top, rect.right, rect.bottom};
                ::DrawTextW(dc, kSegmentSeparator, -1, &separator_rect,
                            DT_LEFT | DT_TOP | DT_NOPREFIX | DT_SINGLELINE);
                ::SelectObject(dc, previous);
                x += tight_width(self.measure_dc, kSegmentSeparator, self.fonts.row_value);
            }
            first = false;

            if (!segment.label.empty()) {
                HGDIOBJ const previous = ::SelectObject(dc, self.fonts.row_prefix);
                ::SetTextColor(dc, kTertiary);
                RECT label_rect{x, rect.top, rect.right, rect.bottom};
                std::wstring const label = wide(segment.label);
                ::DrawTextW(dc, label.c_str(), -1, &label_rect,
                            DT_LEFT | DT_TOP | DT_NOPREFIX | DT_SINGLELINE);
                ::SelectObject(dc, previous);
                x += tight_width(self.measure_dc, label, self.fonts.row_prefix) +
                     self.metrics.space_width;
            }

            COLORREF const tone = segment.tone == ReadingTone::Error   ? kError
                                  : segment.tone == ReadingTone::Warn  ? kWarn
                                                                       : kPrimary;
            HGDIOBJ const previous = ::SelectObject(dc, self.fonts.row_value);
            ::SetTextColor(dc, tone);
            RECT value_rect{x, rect.top, rect.right, rect.bottom};
            std::wstring const value = wide(segment.value);
            ::DrawTextW(dc, value.c_str(), -1, &value_rect,
                        DT_LEFT | DT_TOP | DT_NOPREFIX | DT_SINGLELINE);
            ::SelectObject(dc, previous);
            x += tight_width(self.measure_dc, value, self.fonts.row_value);
        }
    }

    if (self.preferences.expanded && !self.detail_text.empty()) {
        HGDIOBJ const previous = ::SelectObject(dc, self.fonts.detail);
        ::SetTextColor(dc, kTertiary);
        RECT detail_rect = self.detail_rect;
        ::DrawTextW(dc, self.detail_text.c_str(), -1, &detail_rect,
                    DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        ::SelectObject(dc, previous);
    }

    paint_border_of(self, dc);

    if (dc != target) {
        ::BitBlt(target, 0, 0, self.width, self.height, dc, 0, 0, SRCCOPY);
    }
    if (buffer != nullptr) {
        if (previous_bitmap != nullptr) {
            ::SelectObject(buffer, previous_bitmap);
        }
        if (bitmap != nullptr) {
            ::DeleteObject(bitmap);
        }
        ::DeleteDC(buffer);
    }
}

void paint_border_of(WidgetState& self, HDC dc) {
    // DWM 圆角生效时整圈边框由系统画（它跟圆角完全对齐、亮度也更高），自绘会在角上错开一像素。
    if (self.dwm_rounded || !self.gdiplus_held || self.width < 2 || self.height < 2) {
        return;
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float const right = static_cast<float>(self.width - 1);
    float const bottom = static_cast<float>(self.height - 1);
    float const diameter = static_cast<float>(kCornerRadius * 2);
    Gdiplus::GraphicsPath path;
    path.AddArc(0.0f, 0.0f, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(right - diameter, 0.0f, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(0.0f, bottom - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
    Gdiplus::Pen pen(Gdiplus::Color(255, 58, 58, 64), 1.0f);
    graphics.DrawPath(&pen, &path);
}

void arm_save_timer_of(WidgetState& self) {
    ::KillTimer(self.hwnd, kTimerSaveState);
    if (::SetTimer(self.hwnd, kTimerSaveState, kSaveDebounceMs, nullptr) == 0) {
        // 定时器配额耗尽：这次锚点改动就没有防抖落盘了。行为不变（锚点仍是 anchor_dirty，
        // 销毁时的兜底会补写一次），但必须留一行证据，不能静默失效。
        log_format(L"widget: cannot arm the anchor save timer (error=%lu); the pending anchor will "
                   L"be flushed on shutdown",
                   ::GetLastError());
    }
}

void place_by_anchor_of(WidgetState& self) {
    SIZE const size{self.width, self.height};
    if (!self.preferences.anchor_known) {
        // 没有保存过位置：默认锚点落在主屏可用区右下角。这是一次真正的偏好变化，
        // 等尺寸稳定后（落盘定时器）写一次。
        self.preferences.anchor = default_anchor_of(self);
        self.preferences.anchor_known = true;
        if (!self.anchor_dirty) {
            self.anchor_dirty = true;
            self.anchor_before = self.preferences;
        }
    }
    POINT const origin = clamp_point_of(
        self, POINT{self.preferences.anchor.x - size.cx, self.preferences.anchor.y - size.cy}, size);
    ::SetWindowPos(self.hwnd, nullptr, origin.x, origin.y, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    arm_save_timer_of(self);
}

void remember_anchor_of(WidgetState& self) {
    RECT rect{};
    if (!::GetWindowRect(self.hwnd, &rect)) {
        return;
    }
    if (!self.anchor_dirty) {
        self.anchor_dirty = true;
        self.anchor_before = self.preferences;
    }
    self.preferences.anchor = POINT{rect.right, rect.bottom};
    self.preferences.anchor_known = true;
}

POINT clamp_point_of(WidgetState const& self, POINT origin, SIZE size) {
    (void)self;
    // 按**左上角**判断在哪块显示器上：锚点正好落在两块屏交界处时，按锚点判断会把窗口甩到隔壁屏。
    HMONITOR const monitor = ::MonitorFromPoint(origin, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT area{};
    if (monitor != nullptr && ::GetMonitorInfoW(monitor, &info)) {
        area = info.rcWork;
    } else {
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    }
    // 上界用 max(area.left, area.right - width)：窗口比屏还宽时不会夹出一个负值。
    int const left =
        std::min(std::max(area.left, origin.x), std::max(area.left, area.right - size.cx));
    int const top =
        std::min(std::max(area.top, origin.y), std::max(area.top, area.bottom - size.cy));
    return POINT{left, top};
}

POINT default_anchor_of(WidgetState const& self) {
    (void)self;
    RECT area{};
    if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0)) {
        area = RECT{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
    }
    return POINT{area.right - kEdgeMargin, area.bottom - kEdgeMargin};
}

void apply_corner_shape_of(WidgetState& self) {
    if (set_corner_preference_of(self, kDwmcpRound)) {
        self.dwm_rounded = true;
        ::SetWindowRgn(self.hwnd, nullptr, TRUE);
        return;
    }
    // 老系统：自己裁窗口区域（硬边），边框也由自绘补上。
    self.dwm_rounded = false;
    HRGN const region = ::CreateRoundRectRgn(0, 0, self.width + 1, self.height + 1,
                                             kCornerRadius * 2, kCornerRadius * 2);
    if (region != nullptr) {
        // SetWindowRgn 成功时区域归系统所有，不能再自己删。
        ::SetWindowRgn(self.hwnd, region, TRUE);
    }
}

bool set_corner_preference_of(WidgetState const& self, int preference) {
    if (self.hwnd == nullptr) {
        return false;
    }
    int value = preference;
    return ::DwmSetWindowAttribute(self.hwnd, kDwmWindowCornerPreference, &value, sizeof(value)) ==
           S_OK;
}

void apply_topmost_of(WidgetState& self) {
    ::SetWindowPos(self.hwnd, self.preferences.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool is_topmost_of(WidgetState const& self) {
    ::SetLastError(ERROR_SUCCESS);
    LONG_PTR const style = ::GetWindowLongPtrW(self.hwnd, GWL_EXSTYLE);
    if (style == 0 && ::GetLastError() != ERROR_SUCCESS) {
        return true;  // 读不到就当它是置顶的：宁可不动
    }
    return (style & WS_EX_TOPMOST) != 0;
}

bool overlapped_from_above_of(WidgetState const& self) {
    RECT mine{};
    if (!::GetWindowRect(self.hwnd, &mine)) {
        return false;
    }
    DWORD own = 0;
    ::GetWindowThreadProcessId(self.hwnd, &own);

    HWND cursor = self.hwnd;
    for (int step = 0; step < kTopmostScanLimit; ++step) {
        cursor = ::GetWindow(cursor, GW_HWNDPREV);
        if (cursor == nullptr) {
            return false;
        }
        if (!::IsWindowVisible(cursor)) {
            continue;
        }
        DWORD owner = 0;
        ::GetWindowThreadProcessId(cursor, &owner);
        if (owner == own) {
            continue;  // 本进程自己的悬停提示/菜单不算
        }
        RECT other{};
        if (!::GetWindowRect(cursor, &other)) {
            continue;
        }
        if (other.left < mine.right && mine.left < other.right && other.top < mine.bottom &&
            mine.top < other.bottom) {
            return true;
        }
    }
    return false;
}

// 与 overlapped_from_above 的扫描法互补：那条路要沿 GW_HWNDPREV 一步一步走，被一长串不可见窗口
// 把 kTopmostScanLimit 步吃光就会漏判。这里直接问系统“这一点最上面是谁”，判据与 z 序深度无关。
bool covered_by_foreign_window_of(WidgetState const& self, std::wstring* detail) {
    RECT mine{};
    if (self.hwnd == nullptr || !::GetWindowRect(self.hwnd, &mine)) {
        return false;  // 读不到自己的矩形就当没被压住：这一路宁可漏判，也不乱抬
    }
    int const left = mine.left + kTopmostProbeInset;
    int const top = mine.top + kTopmostProbeInset;
    int const right = mine.right - kTopmostProbeInset;    // 不含
    int const bottom = mine.bottom - kTopmostProbeInset;  // 不含
    POINT const center{(mine.left + mine.right) / 2, (mine.top + mine.bottom) / 2};

    // 四角各内缩 3 像素 + 中心：矩形太小（内缩后连两个不同的角都取不到）就只取中心。
    POINT samples[5] = {center, center, center, center, center};
    if (right - left >= kTopmostProbeMinSize && bottom - top >= kTopmostProbeMinSize) {
        samples[0] = POINT{left, top};
        samples[1] = POINT{right - 1, top};
        samples[2] = POINT{left, bottom - 1};
        samples[3] = POINT{right - 1, bottom - 1};
    }

    DWORD const own = window_process_id(self.hwnd);
    for (POINT const& sample : samples) {
        HWND const hit = ::WindowFromPoint(sample);
        if (hit == nullptr) {
            continue;  // 这一点上没有窗口（罕见）：不当成被压住
        }
        // 顶层窗口才是“谁盖住了我们”：命中的可能是某个顶层窗口的子控件。
        HWND const top = ::GetAncestor(hit, GA_ROOT);
        if (top == nullptr) {
            continue;
        }
        if (top == self.hwnd) {
            continue;  // 这一点最上面就是我们自己，没被压住
        }
        if (window_process_id(top) == own) {
            continue;  // 本进程自己的提示条/菜单不算
        }
        if (!::IsWindowVisible(top)) {
            continue;  // 看不见的窗口盖不住任何东西（扫描法里也是同一个判据）
        }
        if (detail != nullptr) {
            *detail =
                window_class_name(top) + L" " + hex_text(reinterpret_cast<unsigned long long>(top));
        }
        return true;
    }
    return false;
}

void verify_topmost_of(WidgetState& self) {
    if (!self.preferences.topmost) {
        return;
    }
    // 两个判据任一成立就补置顶：命中测试（确定性，主判据）与 z 序扫描（兜底，对“置顶位被外部
    // 摘掉”这类邻近情形也敏感）。都不成立就一次都不碰，否则就是“无条件每秒重抬”。
    //
    // 顺序有讲究：命中测试必须排在前面。它不只是主判据，还是 cover= 那个 detail 的唯一来源；
    // 而 || 是短路的，扫描法一旦先返回 true，命中测试根本不会执行，日志就只剩下没有 detail 的
    // 那一句。两个都是纯查询，谁先谁后都不改变 covered 的取值。
    std::wstring detail;
    bool const covered = covered_by_foreign_window_of(self, &detail) || overlapped_from_above_of(self);
    if (is_topmost_of(self) && !covered) {
        // 上一 tick 还压着、这一 tick 已经没事了：清掉节流状态，下次被压住立刻记一行。
        self.topmost_cover_log_ms = 0;
        return;
    }
    // 实测把 WS_EX_TOPMOST 摘掉后窗口自己不会发现：这个守护是唯一的补救。
    // 窗口被隐藏时照样补置顶位，但不记日志（藏着的窗口本来就容易被别的窗口盖住）。
    if (::IsWindowVisible(self.hwnd)) {
        unsigned long long const now = now_ms();
        if (self.topmost_cover_log_ms == 0 ||
            now - self.topmost_cover_log_ms >= kTopmostCoverLogMs) {
            self.topmost_cover_log_ms = now;
            if (covered && !detail.empty()) {
                log_format(L"widget: the topmost bit was lost or something covers us; "
                           L"reapplying it (cover=%s)",
                           detail.c_str());
            } else {
                log_line(L"widget: the topmost bit was lost or something covers us; reapplying it");
            }
        }
    }
    apply_topmost_of(self);
}

bool save_preferences_of(WidgetState& self, WindowPreferences const& before) {
    if (self.callbacks.save_preferences == nullptr) {
        // 主进程没接落盘通道：只改内存。宁可这次运行记得住，也不要拦着用户操作。
        return true;
    }
    std::wstring error;
    if (self.callbacks.save_preferences(self.callbacks.context, self.preferences, &error)) {
        return true;
    }
    // 落盘失败：回滚内存里的偏好，让界面与磁盘一致（不把没落盘的状态显示成已生效）。
    self.preferences = before;
    log_format(L"widget: cannot persist the window preferences (%s); the change was rolled back",
               error.c_str());
    return false;
}

void set_user_hidden_of(WidgetState& self, bool hidden) {
    WindowPreferences const before = self.preferences;
    self.preferences.hidden = hidden;
    if (!save_preferences_of(self, before)) {
        return;  // 已回滚，可见性一个像素都不动
    }
    if (hidden) {
        ::ShowWindow(self.hwnd, SW_HIDE);
        record_status_of(self, L"隐藏", L"用户从菜单隐藏了悬浮窗");
        log_line(L"widget: hidden from the menu (process, taskbar component and fetch loop keep "
                 L"running)");
        return;
    }
    // 从菜单叫回来：与任务栏那块叫它出来走同一条路。
    if (self.fullscreen || self.options.force_hidden || !self.has_content) {
        record_status_of(self, L"隐藏", L"隐藏标记已清，但当前不显示（全屏让位 / --hidden / 暂无内容）");
        return;
    }
    self.ready_to_show = true;
    place_by_anchor_of(self);
    ::ShowWindow(self.hwnd, SW_SHOWNOACTIVATE);
    record_status_of(self, L"显示", content_reason_of(self) + L"（用户从菜单恢复了悬浮窗）");
}

void set_expanded_of(WidgetState& self, bool expanded) {
    WindowPreferences const before = self.preferences;
    self.preferences.expanded = expanded;
    rebuild_lines_of(self);
    relayout_of(self);
    update_tooltips_of(self);
    if (!save_preferences_of(self, before)) {
        // 落盘失败：回滚并重新排版回原来的样子。
        rebuild_lines_of(self);
        relayout_of(self);
        update_tooltips_of(self);
        return;
    }
    if (self.menu != nullptr) {
        ::ModifyMenuW(self.menu, kCmdExpanded, MF_BYCOMMAND | MF_STRING, kCmdExpanded,
                      self.preferences.expanded ? L"切换至精简" : L"切换至详细");
        ::CheckMenuItem(self.menu, kCmdExpanded, MF_BYCOMMAND | MF_UNCHECKED);
    }
}

void record_status_of(WidgetState& self, std::wstring const& state, std::wstring const& reason) {
    std::wstring const key = state + L"：" + reason;
    if (key == self.last_status) {
        return;
    }
    self.last_status = key;
    log_format(L"widget: %s（%s）", state.c_str(), reason.c_str());
}

void on_lbutton_down_of(WidgetState& self, POINT screen) {
    self.dragging = true;
    self.drag_moved = false;
    self.drag_from = screen;
    self.drag_before = self.preferences;
    RECT rect{};
    ::GetWindowRect(self.hwnd, &rect);
    self.drag_origin = POINT{rect.left, rect.top};
    ::SetCapture(self.hwnd);
}

void on_mouse_move_of(WidgetState& self, POINT screen) {
    if (!self.dragging) {
        return;
    }
    int const dx = screen.x - self.drag_from.x;
    int const dy = screen.y - self.drag_from.y;
    if (!self.drag_moved &&
        (dx > kDragSlop || dx < -kDragSlop || dy > kDragSlop || dy < -kDragSlop)) {
        self.drag_moved = true;
    }
    if (!self.drag_moved) {
        return;
    }
    POINT const origin = clamp_point_of(
        self, POINT{self.drag_origin.x + dx, self.drag_origin.y + dy}, SIZE{self.width, self.height});
    ::SetWindowPos(self.hwnd, nullptr, origin.x, origin.y, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    remember_anchor_of(self);
}

// 一次“真的移动过”的拖动收尾：记住新的右下角锚点并立刻落盘（落盘失败就按旧的角重新落位）。
// 鼠标抬起与被外部抢走捕获（WM_CAPTURECHANGED）都走这一条，两条路的行为必须一致。
void finish_drag_after_move_of(WidgetState& self) {
    remember_anchor_of(self);
    ::KillTimer(self.hwnd, kTimerSaveState);
    self.anchor_dirty = false;
    if (!save_preferences_of(self, self.drag_before)) {
        // 落盘失败：锚点已回滚，窗口按旧的角重新落位。
        place_by_anchor_of(self);
    }
}

void on_lbutton_up_of(WidgetState& self, POINT screen) {
    if (!self.dragging) {
        return;
    }
    bool const moved = self.drag_moved;
    // 先落旗再 ReleaseCapture：释放捕获会同步送来 WM_CAPTURECHANGED，那一条不该再收尾一次。
    self.dragging = false;
    ::ReleaseCapture();
    if (moved) {
        finish_drag_after_move_of(self);
        return;
    }
    // 没挪动：自己判双击（不依赖现成控件的双击事件，它会在拖动中途误触）。
    unsigned long long const now = now_ms();
    bool const double_tap = self.last_tap_ms != 0 && now - self.last_tap_ms <= kDoubleTapMs &&
                            screen.x - self.last_tap_point.x <= kDoubleTapSlop &&
                            self.last_tap_point.x - screen.x <= kDoubleTapSlop &&
                            screen.y - self.last_tap_point.y <= kDoubleTapSlop &&
                            self.last_tap_point.y - screen.y <= kDoubleTapSlop;
    if (double_tap) {
        self.last_tap_ms = 0;
        set_expanded_of(self, !self.preferences.expanded);
        return;
    }
    self.last_tap_ms = now;
    self.last_tap_point = screen;
}

void on_rbutton_up_of(WidgetState& self, POINT client) {
    (void)client;
    // 记下这次手势的时刻：Windows 可能紧接着补一条 WM_CONTEXTMENU，那一条要按这个时刻去重。
    self.last_menu_ms = now_ms();
    show_context_menu_of(self);
}

void show_context_menu_of(WidgetState& self) {
    if (self.menu == nullptr) {
        return;
    }
    // 菜单打开时按当前状态回填。
    ::CheckMenuItem(self.menu, kCmdTopmost,
                    MF_BYCOMMAND | (self.preferences.topmost ? MF_CHECKED : MF_UNCHECKED));
    ::ModifyMenuW(self.menu, kCmdExpanded, MF_BYCOMMAND | MF_STRING, kCmdExpanded,
                  self.preferences.expanded ? L"切换至精简" : L"切换至详细");
    ::CheckMenuItem(self.menu, kCmdExpanded, MF_BYCOMMAND | MF_UNCHECKED);
    ::ModifyMenuW(self.menu, kCmdHide, MF_BYCOMMAND | MF_STRING, kCmdHide,
                  self.preferences.hidden ? L"显示悬浮窗" : L"隐藏悬浮窗");
    ::CheckMenuItem(self.menu, kCmdHide, MF_BYCOMMAND | MF_UNCHECKED);
    ::CheckMenuItem(self.menu, kCmdTaskbarComponent,
                    MF_BYCOMMAND |
                        (self.preferences.taskbar_component ? MF_CHECKED : MF_UNCHECKED));

    // 弹出前把这一份文案记一行：菜单是模态的、外面看不见它，出问题时“菜单上到底写的什么”
    // 就只剩这一行证据（离线夹具也靠它核对动态文案与勾选，不必去抓那个抓不住的弹出菜单）。
    log_format(L"widget: context menu texts: expanded=<%s> hide=<%s> topmost=%s "
               L"taskbar_component=%s",
               self.preferences.expanded ? L"切换至精简" : L"切换至详细",
               self.preferences.hidden ? L"显示悬浮窗" : L"隐藏悬浮窗",
               self.preferences.topmost ? L"checked" : L"unchecked",
               self.preferences.taskbar_component ? L"checked" : L"unchecked");

    POINT cursor{};
    ::GetCursorPos(&cursor);
    // 不先把自己设成前台窗口的话，菜单点到别处不会收起来。
    ::SetForegroundWindow(self.hwnd);
    UINT const command = ::TrackPopupMenu(self.menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                          cursor.x, cursor.y, 0, self.hwnd, nullptr);
    // 官方 sample 要求的收尾：菜单之后补一条 WM_NULL。少了它，下一次弹出可能立刻消失
    // （模态循环退出时残留的鼠标状态会被当成一次点击）。这条消息对 DefWindowProc 是无操作。
    ::PostMessageW(self.hwnd, WM_NULL, 0, 0);
    if (command == 0) {
        return;
    }
    run_command_of(self, command);
}

void run_command_of(WidgetState& self, UINT command) {
    switch (command) {
        case kCmdRefresh:
            // 只是请求主进程立刻取一次数：不改地址、不改调度配置、不重绑。
            if (self.callbacks.refresh != nullptr) {
                self.callbacks.refresh(self.callbacks.context);
            }
            break;
        case kCmdTopmost: {
            // 置顶只是改窗口的系统状态；菜单上的勾与偏好文件都是缓存下来的意图。
            // 翻转之后每秒的守护（verify_topmost）按新状态继续工作：关掉时不重抬，打开时照旧守。
            WindowPreferences const before = self.preferences;
            self.preferences.topmost = !self.preferences.topmost;
            apply_topmost_of(self);
            if (!save_preferences_of(self, before)) {
                apply_topmost_of(self);  // 已回滚：把窗口的系统状态也改回去
            }
            if (self.menu != nullptr) {
                ::CheckMenuItem(self.menu, kCmdTopmost,
                                MF_BYCOMMAND |
                                    (self.preferences.topmost ? MF_CHECKED : MF_UNCHECKED));
            }
            log_format(L"widget: the topmost switch -> %s",
                       self.preferences.topmost ? L"on" : L"off");
            break;
        }
        case kCmdExpanded:
            set_expanded_of(self, !self.preferences.expanded);
            break;
        case kCmdTaskbarComponent: {
            // 开关压得住守护循环这件事在主进程那边；窗口只管把意图递出去、拿回**切换之后**的
            // 实际状态，并按它更新勾选。任务栏组件的持久化由主进程在那个动作里做一次。
            bool const wanted = !self.preferences.taskbar_component;
            bool reached = wanted;
            if (self.callbacks.set_taskbar_component != nullptr) {
                reached = self.callbacks.set_taskbar_component(self.callbacks.context, wanted);
            }
            self.preferences.taskbar_component = reached;
            if (self.menu != nullptr) {
                ::CheckMenuItem(self.menu, kCmdTaskbarComponent,
                                MF_BYCOMMAND | (reached ? MF_CHECKED : MF_UNCHECKED));
            }
            log_format(L"widget: the taskbar component switch -> %s", reached ? L"on" : L"off");
            break;
        }
        case kCmdHide:
            // 开关：当前藏着就把它叫出来，当前显示着就藏起来。菜单文案在弹出时已经改好。
            if (self.preferences.hidden) {
                set_user_hidden_of(self, false);
            } else {
                set_user_hidden_of(self, true);
            }
            break;
        case kCmdExit:
            if (self.callbacks.quit != nullptr) {
                self.callbacks.quit(self.callbacks.context);
            } else {
                ::DestroyWindow(self.hwnd);
            }
            break;
        default:
            break;
    }
}

void teardown_window_of(WidgetState& self) {
    if (self.hwnd != nullptr) {
        // 计时器只在窗口还活着时碰：窗口已经没了的时候那个句柄可能已经被系统复用。
        ::KillTimer(self.hwnd, kTimerTopmost);
        ::KillTimer(self.hwnd, kTimerSaveState);
    }
    // 提示是窗口的子窗口，但显式先拆掉：句柄要退休，不能留到下一次建窗再被 FindWindowEx 捡到。
    if (self.tip != nullptr) {
        ::DestroyWindow(self.tip);
        self.tip = nullptr;
    }
    if (self.menu != nullptr) {
        ::DestroyMenu(self.menu);
        self.menu = nullptr;
    }
    if (self.hwnd != nullptr) {
        HWND const hwnd = self.hwnd;
        self.hwnd = nullptr;
        ::DestroyWindow(hwnd);
    }
}

void release_resources_of(WidgetState& self) {
    if (self.surface_brush != nullptr) {
        ::DeleteObject(self.surface_brush);
        self.surface_brush = nullptr;
    }
    if (self.border_pen != nullptr) {
        ::DeleteObject(self.border_pen);
        self.border_pen = nullptr;
    }
    if (self.measure_dc != nullptr) {
        ::DeleteDC(self.measure_dc);
        self.measure_dc = nullptr;
    }
    destroy_fonts(self.fonts);
    if (self.gdiplus_held) {
        gdiplus_release();
        self.gdiplus_held = false;
    }
}

void destroy_window_of(WidgetState& self) {
    // 收尾不再重写整份设置：可切换状态在切换那一刻就已经落过盘。这里只补**还欠着的那一次**
    // ——锚点改过、防抖定时器还没到点。窗口已经被外部 DestroyWindow 掉时一个字节都不写
    // （那时再写就是写陈旧偏好）。窗口都要没了，没有可回滚的 before，失败只记一行日志，
    // 不谎称“已回滚”。
    if (self.hwnd != nullptr && self.anchor_dirty) {
        self.anchor_dirty = false;
        std::wstring error;
        if (self.callbacks.save_preferences != nullptr &&
            !self.callbacks.save_preferences(self.callbacks.context, self.preferences, &error)) {
            log_format(L"widget: the pending anchor could not be persisted on shutdown (%s)",
                       error.c_str());
        }
    }
    teardown_window_of(self);
    release_resources_of(self);
}

}  // namespace

// 公开头里的那个不透明类型：只是上面 WidgetState 的一次空包装，所以 window.h 不必随内部改动而变。
struct WidgetWindow::Impl : WidgetState {};

WidgetWindow::~WidgetWindow() { destroy(); }

bool WidgetWindow::create(WidgetOptions const& options, WidgetCallbacks const& callbacks,
                          std::wstring* error) {
    if (impl_ != nullptr || hwnd_ != nullptr || options.instance == nullptr ||
        options.class_name.empty()) {
        if (error != nullptr) {
            *error = L"悬浮窗参数不完整（instance / class_name 不能为空），或这个对象已经建过窗";
        }
        return false;
    }

    // 候选实现：全部建成功才 adopt。任何失败路径都在本地收干净（窗口、提示、菜单、GDI 资源、
    // GDI+ 引用），**一个字节的界面偏好都不写**；对象保持“没建过”的状态，所以失败之后还能重试，
    // 析构也不会触发一次空落盘。
    auto* candidate = new Impl();
    candidate->options = options;
    candidate->callbacks = callbacks;
    candidate->preferences = options.preferences;

    bool built = false;
    do {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &WidgetWindow::window_proc;
        wc.hInstance = options.instance;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = options.class_name.c_str();
        if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            if (error != nullptr) {
                *error = L"注册悬浮窗窗口类失败: " + last_error_text();
            }
            break;
        }

        // 没有标题栏、不在任务栏里；隐藏着建，拿到第一份状态之后再决定要不要显示。
        DWORD const ex_style =
            WS_EX_TOOLWINDOW | (candidate->preferences.topmost ? WS_EX_TOPMOST : 0);
        HWND const hwnd =
            ::CreateWindowExW(ex_style, options.class_name.c_str(), kWindowTitle, WS_POPUP, 0, 0, 0,
                              0, nullptr, nullptr, options.instance, this);
        if (hwnd == nullptr) {
            if (error != nullptr) {
                *error = L"创建悬浮窗失败: " + last_error_text();
            }
            break;
        }
        // window_proc 在 WM_NCCREATE 里已经把 hwnd_ 指向这个窗口；这里同步候选自己的句柄。
        candidate->hwnd = hwnd;

        candidate->gdiplus_held = gdiplus_acquire();
        candidate->measure_dc = ::CreateCompatibleDC(nullptr);
        if (candidate->measure_dc == nullptr) {
            if (error != nullptr) {
                *error = L"取不到用于量文本的设备上下文: " + last_error_text();
            }
            break;
        }
        if (!create_fonts(candidate->measure_dc, candidate->fonts)) {
            if (error != nullptr) {
                *error = L"建字体失败（Microsoft YaHei UI / Microsoft YaHei / Segoe UI 都不可用）";
            }
            break;
        }
        compute_text_metrics(candidate->measure_dc, candidate->fonts, candidate->metrics,
                             candidate->gdiplus_held);
        log_format(L"widget: face=%s (%s) row_height=%d space=%d detail_line_height=%d",
                   candidate->fonts.face.c_str(), candidate->fonts.source.c_str(),
                   candidate->metrics.row_height, candidate->metrics.space_width,
                   candidate->metrics.detail_line_height);

        candidate->surface_brush = ::CreateSolidBrush(kSurface);
        candidate->border_pen = ::CreatePen(PS_SOLID, 1, kBorder);
        create_tooltip_of(*candidate);
        create_context_menu_of(*candidate);
        apply_corner_shape_of(*candidate);
        apply_topmost_of(*candidate);
        if (::SetTimer(candidate->hwnd, kTimerTopmost, kTopmostGuardMs, nullptr) == 0) {
            // 定时器配额耗尽：置顶守护与全屏让位会静默失效。宁可建窗失败让调用方看见，
            // 也不许假装守护已经挂上（失败路径会把已经建出来的窗口与资源收干净）。
            if (error != nullptr) {
                *error = L"装不上置顶守护定时器: " + last_error_text();
            }
            break;
        }
        built = true;
    } while (false);

    if (!built) {
        // 失败清理：先让 window_proc 不再把这个候选当成活着的窗口，再拆窗口与资源，不落盘。
        hwnd_ = nullptr;
        teardown_window_of(*candidate);
        release_resources_of(*candidate);
        delete candidate;
        return false;
    }

    impl_ = candidate;
    hwnd_ = candidate->hwnd;
    return true;
}

LRESULT CALLBACK WidgetWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto const* create = reinterpret_cast<CREATESTRUCTW const*>(lparam);
        auto* self = static_cast<WidgetWindow*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->hwnd_ = hwnd;
        }
    }
    auto* self = reinterpret_cast<WidgetWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // WM_NCDESTROY 单独处理，而且**不经过 impl_ 是否还在的判断**：destroy() 会先把 impl_ 摘掉
    // 再销毁窗口，外部 DestroyWindow（例如没有 quit 回调时 WM_CLOSE 的兜底）更是完全绕过
    // destroy()。两种情况都必须在这里把两个持有者里的这个句柄作废并清掉 USERDATA，否则
    // handle() 会继续返回一个已经销毁的句柄，之后 apply()/destroy() 就可能作用到被系统复用
    // 的那个句柄上。这里只认传进来的实际 hwnd，不去读可能已经失效的成员。
    if (message == WM_NCDESTROY) {
        if (self != nullptr) {
            if (self->impl_ != nullptr && self->impl_->hwnd == hwnd) {
                self->impl_->hwnd = nullptr;
            }
            if (self->hwnd_ == hwnd) {
                self->hwnd_ = nullptr;
            }
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return self->handle_message(message, wparam, lparam);
}

LRESULT WidgetWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (impl_ == nullptr) {
        return ::DefWindowProcW(hwnd_, message, wparam, lparam);
    }
    WidgetState& self = *impl_;
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint_struct{};
            HDC const dc = ::BeginPaint(hwnd_, &paint_struct);
            paint_of(self, dc);
            ::EndPaint(hwnd_, &paint_struct);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ::ClientToScreen(hwnd_, &point);
            on_lbutton_down_of(self, point);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!self.dragging) {
                return 0;
            }
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ::ClientToScreen(hwnd_, &point);
            on_mouse_move_of(self, point);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ::ClientToScreen(hwnd_, &point);
            on_lbutton_up_of(self, point);
            return 0;
        }
        case WM_RBUTTONUP:
            on_rbutton_up_of(self, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_CAPTURECHANGED:
            // 捕获被外部抢走（Alt+Tab、别的窗口 SetCapture、系统弹窗）：结束这次拖动，免得指针
            // 再移进窗口时在**没按键**的情况下把窗口拖走。已经移动过就按“拖到哪算哪”收尾
            // （记住锚点并落盘），与鼠标抬起同一条路。
            if (self.dragging) {
                self.dragging = false;
                if (self.drag_moved) {
                    finish_drag_after_move_of(self);
                }
            }
            return 0;
        case WM_TIMER:
            if (wparam == kTimerTopmost) {
                // 全屏状态机先跑：它可能刚把窗口藏起来，也可能刚把它放出来。
                update_fullscreen_state_of(self);
                if (self.fullscreen) {
                    // 全屏期间不给置顶守护动手：那时窗口本来就藏着，而且“有东西盖住我们”
                    // 这个判据在全屏窗口面前必然成立，一分钟六十次 SetWindowPos 全是白费。
                    return 0;
                }
                verify_topmost_of(self);
                return 0;
            }
            if (wparam == kTimerSaveState) {
                ::KillTimer(hwnd_, kTimerSaveState);
                // 锚点在拖动/首次落位时改过：现在尺寸稳定了，落盘一次。
                if (self.anchor_dirty) {
                    self.anchor_dirty = false;
                    WindowPreferences const before = self.anchor_before;
                    if (!save_preferences_of(self, before)) {
                        // 落盘失败：锚点回滚，窗口按旧的角重新落位，界面与磁盘保持一致。
                        place_by_anchor_of(self);
                    }
                }
                return 0;
            }
            return 0;
        case WM_CONTEXTMENU:
            // 键盘的「菜单键」（VK_APPS）/ Shift+F10 走这条消息；真实右键则先到上面的
            // WM_RBUTTONUP，Windows 可能紧接着补一条 WM_CONTEXTMENU。两条路都指向同一个菜单，
            // 所以这里按时间戳去重，避免同一个手势弹两次。
            if (self.last_menu_ms == 0 || now_ms() - self.last_menu_ms > kMenuRepeatMs) {
                self.last_menu_ms = now_ms();
                show_context_menu_of(self);
            }
            return 0;
        case WM_COMMAND:
            // 菜单本身用 TrackPopupMenu(TPM_RETURNCMD)，选中一项不会发 WM_COMMAND——那条路由
            // show_context_menu 直接调 run_command。这里接的是“从外面按命令号递进来”的同一条
            // 命令，离线夹具靠它无人值守地逐条验证菜单项。
            run_command_of(self, static_cast<UINT>(wparam & 0xFFFFu));
            return 0;
        case WM_CLOSE:
            // Alt+F4 与菜单「退出」走同一条收尾路径：外面按“先摘任务栏负载、再关窗”的顺序来。
            if (self.callbacks.quit != nullptr) {
                self.callbacks.quit(self.callbacks.context);
            } else {
                ::DestroyWindow(hwnd_);
            }
            return 0;
        default:
            return ::DefWindowProcW(hwnd_, message, wparam, lparam);
    }
}

void WidgetWindow::apply(Frame const& frame) {
    if (impl_ == nullptr) {
        return;
    }
    WidgetState& self = *impl_;
    if (self.hwnd == nullptr) {
        // 窗口已经被外部销毁（例如没有 quit 回调时 WM_CLOSE 的兜底）：不再做任何 Win32 调用，
        // 免得作用到被系统复用的句柄上。主进程随后 destroy() 收尾即可。
        return;
    }
    self.kind = frame.kind;
    if (frame.kind != ViewKind::Waiting) {
        // 只要主进程给出过一份状态（哪怕是没有条目的清空），就算“拿到过”。
        self.rendered_once = true;
    }
    self.content = reading_from_frame(frame);
    self.has_content = !self.content.rows.empty();
    rebuild_lines_of(self);
    relayout_of(self);
    update_tooltips_of(self);

    std::wstring const reason = content_reason_of(self);
    if (!self.has_content) {
        // 等待首份快照，或合法的空条目：整窗隐藏，不露空窗。
        record_status_of(self, L"隐藏", reason);
        ::ShowWindow(hwnd_, SW_HIDE);
        return;
    }
    if (self.fullscreen) {
        // 这一条是系统状态，不是用户意图；用户“隐藏”与它同时成立时显示的也应该是这一条。
        record_status_of(self, L"隐藏", reason + L"（全屏程序占着屏幕）");
        return;
    }
    if (self.preferences.hidden) {
        record_status_of(self, L"隐藏", reason + L"（用户从菜单隐藏了悬浮窗）");
        return;
    }
    if (self.options.force_hidden) {
        record_status_of(self, L"隐藏", reason + L"（--hidden：本次运行强制隐藏）");
        return;
    }
    record_status_of(self, L"显示", reason);
    if (!::IsWindowVisible(hwnd_)) {
        self.ready_to_show = true;
        place_by_anchor_of(self);
        // 不激活窗口：本进程是常驻宿主，可能在用户正打字时由一次取数把窗口带出来；
        // 置顶与“看得见”都不需要抢焦点。
        ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
}

void WidgetWindow::show_from_request() {
    if (impl_ == nullptr) {
        return;
    }
    WidgetState& self = *impl_;
    if (self.hwnd == nullptr) {
        // 窗口已经不在：不做任何 Win32 调用（与 apply 同一条理由）。
        return;
    }
    if (self.options.force_hidden) {
        // --hidden 是本次运行的强制隐藏：连用户的隐藏标记都不动。一次诊断运行不该改写
        // 用户存下来的 `[window] hidden`。
        log_line(L"widget: a show request arrived but --hidden is in effect for this run");
        record_status_of(self, L"隐藏", L"--hidden：本次运行强制隐藏，显示请求被忽略");
        return;
    }
    WindowPreferences const before = self.preferences;
    self.preferences.hidden = false;
    if (!save_preferences_of(self, before)) {
        // 落盘失败：隐藏标记已回滚，界面保持与磁盘一致（继续藏着）。
        log_line(L"widget: a show request arrived but the preference could not be persisted; "
                 L"staying hidden");
        return;
    }
    if (self.fullscreen) {
        // 全屏还没结束：只把用户的意图记下来（隐藏标记已清），窗口等全屏结束再出来。
        log_line(L"widget: a show request arrived while a fullscreen application owns the screen; "
                 L"it will show when the fullscreen application is gone");
        record_status_of(self, L"隐藏", L"前台是全屏程序，显示请求等它退出后再显示");
        return;
    }
    if (!self.rendered_once || !self.has_content) {
        // 还没拿到第一份有内容的快照：拿到之后它自己会显示（隐藏标记已经清掉了）。
        log_line(L"widget: a show request arrived before the first content; it will show when the "
                 L"first content lands");
        return;
    }
    self.ready_to_show = true;
    place_by_anchor_of(self);
    ::ShowWindow(hwnd_, SW_SHOWNORMAL);
    // 前台权是系统给的：调用方不在前台时 Windows 会拒绝（窗口照旧显示，因为它本来就是置顶的）。
    BOOL const raised = ::SetForegroundWindow(hwnd_);
    log_format(L"widget: shown on request from another process (foreground=%s)",
               raised != FALSE ? L"yes" : L"no");
}

void WidgetWindow::destroy() {
    // 幂等：建窗失败（impl_ 与 hwnd_ 都空）之后调用它什么都不做，更不会写任何界面偏好。
    Impl* const impl = impl_;
    HWND const window = hwnd_;
    impl_ = nullptr;
    hwnd_ = nullptr;
    if (impl != nullptr) {
        // destroy_window_of 自己判断窗口还在不在：外部已经 DestroyWindow 过时它只放资源，
        // 不再落盘（那时再写一次就是写陈旧偏好）。
        destroy_window_of(*impl);
        delete impl;
        return;
    }
    if (window != nullptr) {
        // 没有实现状态却还留着一个句柄（理论上不该出现）：也把它收掉。
        ::DestroyWindow(window);
    }
}

}  // namespace deskfeed::widget
