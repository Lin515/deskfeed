#pragma once

#include <windows.h>

#include <string>

// 悬浮窗的配色与字体。
//
// **整窗没有粗体**：前缀与读数用同一字族、同一字号、同一字重，只用取色区分（弱色 / 主色）；
// 生产端的 `warn` 有自己的一档颜色，不会被折成正常色或错误色。
namespace deskfeed::widget {

constexpr COLORREF kSurface = RGB(28, 28, 32);
constexpr COLORREF kBorder = RGB(58, 58, 64);
constexpr COLORREF kPrimary = RGB(242, 242, 244);
constexpr COLORREF kTertiary = RGB(139, 139, 147);
// 警告档：深色底上的琥珀色，与主色、错误色都有足够区分度。
constexpr COLORREF kWarn = RGB(226, 168, 62);
constexpr COLORREF kError = RGB(213, 73, 65);

// 版面常量。
constexpr int kPaddingLeft = 9;
constexpr int kPaddingTop = 6;
constexpr int kPaddingRight = 9;
constexpr int kPaddingBottom = 6;
constexpr int kRowGap = 1;
constexpr int kDetailGap = 7;
constexpr int kCornerRadius = 8;
constexpr int kDetailMaxWidth = 340;

constexpr double kRowFontPoints = 9.0;
constexpr double kDetailFontPoints = 8.25;

// 三个字体。9pt 用于前缀与读数、8.25pt 用于详情——**三个都是常规字重**。
// 前缀与读数分开两个句柄，是为了各自能独立取色、量宽与将来分开调，而不是为了分字重。
struct FontSet {
    HFONT row_prefix = nullptr;
    HFONT row_value = nullptr;
    HFONT detail = nullptr;
    std::wstring face;    // 真正用上的字族（按顺序试出来的那一个）
    std::wstring source;  // 这个字族是怎么定下来的（写日志用）
    // 这两个字体各是多少 pt。正常路径就是 kRowFontPoints / kDetailFontPoints；退回系统消息框
    // 字体时由它的 lfHeight 反算，量行高要用。
    double row_points = kRowFontPoints;
    double detail_points = kDetailFontPoints;
};

// 按顺序挑字族：Microsoft YaHei UI -> Microsoft YaHei -> Segoe UI -> 系统消息框字体。
// 每个候选都靠 GetTextFaceW 回读校验（CreateFont 找不到字族会静默替换，不回读就等于没试）。
// 返回 false 只在三个字体都建不出来时发生（此时窗口不该继续跑）。
bool create_fonts(HDC dc, FontSet& fonts);
void destroy_fonts(FontSet& fonts);

// 行高要的是 .NET `Font.Height` 的口径：em 像素数 × 字族的行距，最后向上取整。
// 同一个 9pt 的 Microsoft YaHei UI，GDI+ 量出 16，GDI 的 tmHeight 是 17——差这一像素，
// 三行窗口就是 66 而不是 63。用法族名 + pt 重建一个 GDI+ 字族来量。
// 返回 0 表示量不出来（GDI+ 没起来 / 字族不在 / dpi 不合法），调用方回退到 GDI 的 tmHeight。
int gdiplus_line_height(std::wstring const& face, double points, int dpi, bool bold);

}  // namespace deskfeed::widget
