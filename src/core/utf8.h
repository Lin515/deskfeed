#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// UTF-8 与文本判据。纯函数，不依赖 Windows，供协议校验与测试使用。
namespace deskfeed {

using CodePoint = std::uint32_t;

// 严格校验：不接受过长编码、代理项、U+10FFFF 以上的码位。
bool utf8_validate(std::string_view text);

// 逐码位解码。成功返回 true 并推进 index；失败返回 false，index 不动。
bool utf8_decode(std::string_view text, std::size_t& index, CodePoint& out);

// 解码失败时返回空串（调用方应先跑 utf8_validate）。
std::wstring wide_from_utf8(std::string_view text);
std::string utf8_from_wide(std::wstring_view text);

// Unicode White_Space 属性。
bool is_unicode_whitespace(CodePoint code_point);

// 空串，或只由 Unicode 空白组成。
bool is_blank(std::string_view text);

// 含任一 Unicode 硬换行：CR（U+000D）、LF（U+000A）、VT（U+000B）、FF（U+000C）、
// NEL（U+0085）、LS（U+2028）、PS（U+2029）。协议要求 label/value 是单行字符串；
// 制表符（U+0009）与其它控制字符不在禁止之列。
bool has_line_break(std::string_view text);

}  // namespace deskfeed
