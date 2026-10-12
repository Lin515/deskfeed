#include "core/utf8.h"

#include <windows.h>

namespace deskfeed {
namespace {

// 只做 UTF-8 与 UTF-16 的互转，不引入任何界面依赖。
std::string to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return std::string();
    }
    int const bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return std::string();
    }
    std::string result(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                          bytes, nullptr, nullptr);
    return result;
}

std::wstring to_wide(std::string_view text) {
    if (text.empty()) {
        return std::wstring();
    }
    int const chars = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (chars <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<std::size_t>(chars), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(), chars);
    return result;
}

bool is_continuation(unsigned char byte) { return (byte & 0xC0) == 0x80; }

}  // namespace

bool utf8_decode(std::string_view text, std::size_t& index, CodePoint& out) {
    if (index >= text.size()) {
        return false;
    }
    unsigned char const first = static_cast<unsigned char>(text[index]);
    if (first < 0x80) {
        out = first;
        ++index;
        return true;
    }

    std::size_t length = 0;
    CodePoint code_point = 0;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        code_point = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        code_point = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        code_point = first & 0x07;
    } else {
        return false;
    }
    if (index + length > text.size()) {
        return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        unsigned char const byte = static_cast<unsigned char>(text[index + offset]);
        if (!is_continuation(byte)) {
            return false;
        }
        code_point = (code_point << 6) | (byte & 0x3F);
    }

    // 过长编码、UTF-16 代理项区间、超出 Unicode 上界都拒绝。
    static constexpr CodePoint kMinimum[4] = {0, 0x80, 0x800, 0x10000};
    if (code_point < kMinimum[length - 1] || code_point > 0x10FFFF) {
        return false;
    }
    if (code_point >= 0xD800 && code_point <= 0xDFFF) {
        return false;
    }

    out = code_point;
    index += length;
    return true;
}

bool utf8_validate(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        CodePoint code_point = 0;
        if (!utf8_decode(text, index, code_point)) {
            return false;
        }
        (void)code_point;
    }
    return true;
}

std::wstring wide_from_utf8(std::string_view text) { return to_wide(text); }

std::string utf8_from_wide(std::wstring_view text) { return to_utf8(text); }

bool is_unicode_whitespace(CodePoint code_point) {
    switch (code_point) {
        case 0x0009:  // CHARACTER TABULATION
        case 0x000A:  // LINE FEED
        case 0x000B:  // LINE TABULATION
        case 0x000C:  // FORM FEED
        case 0x000D:  // CARRIAGE RETURN
        case 0x0020:  // SPACE
        case 0x0085:  // NEXT LINE
        case 0x00A0:  // NO-BREAK SPACE
        case 0x1680:  // OGHAM SPACE MARK
        case 0x2028:  // LINE SEPARATOR
        case 0x2029:  // PARAGRAPH SEPARATOR
        case 0x202F:  // NARROW NO-BREAK SPACE
        case 0x205F:  // MEDIUM MATHEMATICAL SPACE
        case 0x3000:  // IDEOGRAPHIC SPACE
            return true;
        default:
            return code_point >= 0x2000 && code_point <= 0x200A;
    }
}

bool is_blank(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        CodePoint code_point = 0;
        if (!utf8_decode(text, index, code_point)) {
            return false;
        }
        if (!is_unicode_whitespace(code_point)) {
            return false;
        }
    }
    return true;
}

bool has_line_break(std::string_view text) {
    // 真正的换行符都在这里：CR（U+000D）、LF（U+000A）、VT（U+000B）、FF（U+000C）、
    // NEL（U+0085）、LINE SEPARATOR（U+2028）、PARAGRAPH SEPARATOR（U+2029）。
    // 制表符与其它控制字符不在此列——协议只要求 label / value 是“单行字符串”，
    // 没有禁止普通制表符。
    std::size_t index = 0;
    while (index < text.size()) {
        CodePoint code_point = 0;
        if (!utf8_decode(text, index, code_point)) {
            return text.find_first_of("\r\n\v\f") != std::string::npos;
        }
        switch (code_point) {
            case 0x000A:
            case 0x000B:
            case 0x000C:
            case 0x000D:
            case 0x0085:
            case 0x2028:
            case 0x2029:
                return true;
            default:
                break;
        }
    }
    return false;
}

}  // namespace deskfeed
