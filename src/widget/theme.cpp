#include "widget/theme.h"

// GDI+ 的头要用 STDMETHOD / IUnknown：先显式带进 objidl.h，否则是一串“未定义基类 IUnknown”。
#include <objidl.h>
#include <gdiplus.h>

#include <cmath>
#include <string>

namespace deskfeed::widget {
namespace {

// 回读字体实际映射到的字族。CreateFontW 找不到字族时会静默替换（通常是 Arial），
// 只有回读才知道这个候选到底成没成。
std::wstring read_back_face(HDC dc, HFONT font) {
    HGDIOBJ const previous = ::SelectObject(dc, font);
    wchar_t buffer[LF_FACESIZE] = {};
    int const length = ::GetTextFaceW(dc, LF_FACESIZE, buffer);
    ::SelectObject(dc, previous);
    if (length <= 1) {
        return std::wstring();
    }
    return std::wstring(buffer, static_cast<size_t>(length - 1));
}

bool same_face(std::wstring const& left, std::wstring const& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

// Point 单位的字号 → 像素高度：-round(points * dpi / 72)。
int points_to_height(HDC dc, double points) {
    int const dpi = ::GetDeviceCaps(dc, LOGPIXELSY);
    int const pixels = static_cast<int>(points * dpi / 72.0 + 0.5);
    return pixels > 0 ? -pixels : -1;
}

HFONT create_face_font(HDC dc, std::wstring const& face, double points, bool bold) {
    return ::CreateFontW(points_to_height(dc, points), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE,
                         FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.c_str());
}

int dpi_of(HDC dc) {
    return dc != nullptr ? ::GetDeviceCaps(dc, LOGPIXELSY) : 0;
}

// 三个候选都没有时的兜底：系统消息框字体。
bool create_fallback_fonts(HDC dc, FontSet& fonts) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        return false;
    }

    LOGFONTW regular = metrics.lfMessageFont;
    fonts.face = regular.lfFaceName;
    fonts.source = L"三个候选字体都没有，退回系统消息框字体";

    // 系统消息框字体是多少 pt 由 lfHeight 反算（lfHeight 是 em 的像素高度，写字号要取正）。
    int const dpi = dpi_of(dc);
    if (dpi > 0 && regular.lfHeight != 0) {
        fonts.row_points = std::abs(static_cast<double>(regular.lfHeight)) * 72.0 / dpi;
        fonts.detail_points = fonts.row_points * kDetailFontPoints / kRowFontPoints;
    }

    // 前缀与读数同一种字重（都是常规）。
    regular.lfWeight = FW_NORMAL;
    fonts.row_prefix = ::CreateFontIndirectW(&regular);
    fonts.row_value = ::CreateFontIndirectW(&regular);

    LOGFONTW detail = metrics.lfMessageFont;
    detail.lfWeight = FW_NORMAL;
    detail.lfHeight = static_cast<LONG>(metrics.lfMessageFont.lfHeight * kDetailFontPoints /
                                        kRowFontPoints);
    fonts.detail = ::CreateFontIndirectW(&detail);

    if (fonts.row_prefix == nullptr || fonts.row_value == nullptr || fonts.detail == nullptr) {
        destroy_fonts(fonts);
        return false;
    }
    return true;
}

}  // namespace

bool create_fonts(HDC dc, FontSet& fonts) {
    destroy_fonts(fonts);

    wchar_t const* candidates[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI"};
    for (wchar_t const* candidate : candidates) {
        HFONT const probe = create_face_font(dc, candidate, kRowFontPoints, false);
        if (probe == nullptr) {
            continue;
        }
        std::wstring const actual = read_back_face(dc, probe);
        if (!actual.empty() && same_face(actual, candidate)) {
            fonts.face = actual;
            fonts.source = L"按顺序试出来的第一个可用字族";
            fonts.row_prefix = probe;
            break;
        }
        ::DeleteObject(probe);
    }

    if (fonts.row_prefix == nullptr) {
        return create_fallback_fonts(dc, fonts);
    }

    // 读数与前缀同一种字重：**这一面整窗没有粗体**。前缀与读数的区分交给取色（弱色 / 主色），
    // 与任务栏读数块的口径一致。
    fonts.row_value = create_face_font(dc, fonts.face, kRowFontPoints, false);
    fonts.detail = create_face_font(dc, fonts.face, kDetailFontPoints, false);
    if (fonts.row_value == nullptr || fonts.detail == nullptr) {
        destroy_fonts(fonts);
        return false;
    }
    return true;
}

void destroy_fonts(FontSet& fonts) {
    HFONT* handles[] = {&fonts.row_prefix, &fonts.row_value, &fonts.detail};
    for (HFONT* handle : handles) {
        if (*handle != nullptr) {
            ::DeleteObject(*handle);
            *handle = nullptr;
        }
    }
}

int gdiplus_line_height(std::wstring const& face, double points, int dpi, bool bold) {
    if (face.empty() || dpi <= 0 || !(points > 0.0)) {
        return 0;
    }
    Gdiplus::FontFamily family(face.c_str());
    if (family.GetLastStatus() != Gdiplus::Ok) {
        return 0;
    }
    INT const style = bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular;
    Gdiplus::REAL const spacing = family.GetLineSpacing(style);
    Gdiplus::REAL const em = family.GetEmHeight(style);
    if (family.GetLastStatus() != Gdiplus::Ok || !(spacing > 0.0f) || !(em > 0.0f)) {
        return 0;
    }
    // Font.Height = ceil(em 像素数 × 行距 / em 方格)。9pt：ceil(12 × 2601/2048) = 16。
    double const em_pixels = points * dpi / 72.0;
    return static_cast<int>(std::ceil(em_pixels * spacing / em));
}

}  // namespace deskfeed::widget
