#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "widget/theme.h"

// 文本量法。
//
// 最要紧的一条：**GDI 量出来的宽度不含 .NET 那层“外延留白”**。旧实现是先量再减 Overhang，
// 因为 .NET 的 MeasureText 不认 NoPadding；C++ 这边 GetTextExtentPoint32W 与 DrawText 都不带
// 留白，再减一次就会让前缀后的空隙变窄几像素。这一层就是那个换算的唯一出处，量法与画法同源。
namespace deskfeed::widget {

struct TextMetrics {
    // 行高 = max(prefixFont.Height, valueFont.Height)，实测 16 像素。
    int row_height = 0;
    // 一个空格的宽度：用**读数**字体量一次，< 1 取 3。
    int space_width = 0;
    int detail_line_height = 0;
};

// gdiplus_available 为 false（GdiplusStartup 失败）时**不得**构造 `Gdiplus::FontFamily`，
// 直接走 GDI 的 `tmHeight` 兜底：那条兜底本来就写好了，只是调用点必须把它挡住——在没初始化的
// GDI+ 上量字是未定义行为。
void compute_text_metrics(HDC dc, FontSet const& fonts, TextMetrics& metrics,
                          bool gdiplus_available);

// 紧凑宽度（单行，不含换行）。
int tight_width(HDC dc, std::wstring const& text, HFONT font);

// 按 '\n' 拆行（行尾 '\r' 去掉）。
std::vector<std::wstring> split_lines(std::wstring const& text);

// 逐行自然宽度（不换行）里的最大值。
int unwrapped_width(HDC dc, std::wstring const& text, HFONT font);

// 每行在 max_width 内自动换行后的总高度（行与行之间不额外加间距）。
int wrapped_height(HDC dc, std::wstring const& text, HFONT font, int max_width);

// 自动换行之后最宽那一行的宽度（不超过 max_width）。窗宽要用它：详情块的宽度是换行后最长
// 那行，不是 max_width。
int wrapped_width(HDC dc, std::wstring const& text, HFONT font, int max_width);

}  // namespace deskfeed::widget
