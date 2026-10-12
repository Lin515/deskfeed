#include "widget/text_metrics.h"

#include <algorithm>

namespace deskfeed::widget {
namespace {

int line_height_of(HDC dc, HFONT font) {
    HGDIOBJ const previous = ::SelectObject(dc, font);
    TEXTMETRICW metrics{};
    int const height = ::GetTextMetricsW(dc, &metrics) ? static_cast<int>(metrics.tmHeight) : 0;
    ::SelectObject(dc, previous);
    return height;
}

// 一个空行在文本里也占一行高；DrawText 对空串量出来是 0，要自己补上。
int wrapped_height_of_line(HDC dc, std::wstring const& line, HFONT font, int max_width) {
    if (line.empty()) {
        return line_height_of(dc, font);
    }
    RECT rect{0, 0, max_width > 0 ? max_width : 1, 0};
    ::DrawTextW(dc, line.c_str(), static_cast<int>(line.size()), &rect,
                DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_LEFT | DT_TOP);
    return rect.bottom - rect.top;
}

}  // namespace

void compute_text_metrics(HDC dc, FontSet const& fonts, TextMetrics& metrics,
                          bool gdiplus_available) {
    // 行高是 GDI+ 的口径（Font.Height），不是 GDI 的 tmHeight：同一个 9pt 字族差一像素。
    // 前缀与读数都是常规字重，两边量的是同一种字重。
    // GDI+ 没起来时一个 GDI+ 调用都不做，直接落到下面的 tmHeight 兜底。
    int row_height = 0;
    if (gdiplus_available) {
        int const dpi = ::GetDeviceCaps(dc, LOGPIXELSY);
        int const prefix_height = gdiplus_line_height(fonts.face, fonts.row_points, dpi, false);
        int const value_height = gdiplus_line_height(fonts.face, fonts.row_points, dpi, false);
        row_height = std::max(prefix_height, value_height);
    }
    if (row_height <= 0) {
        // GDI+ 不在（或字族量不出来）时的退路：GDI 的 tmHeight，比 Font.Height 大一像素。
        row_height =
            std::max(line_height_of(dc, fonts.row_prefix), line_height_of(dc, fonts.row_value));
    }
    metrics.row_height = row_height;

    // 详情走 DrawText 的口径，行高就是 GDI 的 tmHeight。
    metrics.detail_line_height = line_height_of(dc, fonts.detail);

    // 一个空格的宽度：量 "a a" 与 "aa" 的差。两边外延留白是同一个值，相减抵消。
    int const with_space = tight_width(dc, L"a a", fonts.row_value);
    int const without_space = tight_width(dc, L"aa", fonts.row_value);
    int const space = with_space - without_space;
    metrics.space_width = space < 1 ? 3 : space;
}

int tight_width(HDC dc, std::wstring const& text, HFONT font) {
    if (text.empty()) {
        return 0;
    }
    HGDIOBJ const previous = ::SelectObject(dc, font);
    SIZE size{};
    bool const ok = ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    ::SelectObject(dc, previous);
    return ok ? static_cast<int>(size.cx) : 0;
}

std::vector<std::wstring> split_lines(std::wstring const& text) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    for (;;) {
        size_t const at = text.find(L'\n', start);
        std::wstring line =
            at == std::wstring::npos ? text.substr(start) : text.substr(start, at - start);
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        lines.push_back(line);
        if (at == std::wstring::npos) {
            break;
        }
        start = at + 1;
    }
    return lines;
}

int unwrapped_width(HDC dc, std::wstring const& text, HFONT font) {
    int widest = 0;
    for (std::wstring const& line : split_lines(text)) {
        widest = std::max(widest, tight_width(dc, line, font));
    }
    return widest;
}

int wrapped_height(HDC dc, std::wstring const& text, HFONT font, int max_width) {
    HGDIOBJ const previous = ::SelectObject(dc, font);
    int total = 0;
    for (std::wstring const& line : split_lines(text)) {
        total += wrapped_height_of_line(dc, line, font, max_width);
    }
    ::SelectObject(dc, previous);
    return total;
}

int wrapped_width(HDC dc, std::wstring const& text, HFONT font, int max_width) {
    if (text.empty() || max_width <= 0) {
        return 0;
    }
    HGDIOBJ const previous = ::SelectObject(dc, font);
    int widest = 0;
    for (std::wstring const& line : split_lines(text)) {
        if (line.empty()) {
            continue;
        }
        RECT rect{0, 0, max_width, 0};
        // DrawText 在 DT_CALCRECT + DT_WORDBREAK 下把 rect.right 收成换行后最长那行的宽度。
        ::DrawTextW(dc, line.c_str(), static_cast<int>(line.size()), &rect,
                    DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_LEFT | DT_TOP);
        widest = std::max(widest, static_cast<int>(rect.right));
    }
    ::SelectObject(dc, previous);
    return std::min(widest, max_width);
}

}  // namespace deskfeed::widget
