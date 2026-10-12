#include "core/json.h"

#include "core/utf8.h"

#include <climits>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace deskfeed {

JsonValue const* JsonValue::member(std::string_view name) const {
    for (auto const& entry : members) {
        if (entry.first == name) {
            return &entry.second;
        }
    }
    return nullptr;
}

namespace {

// 把 JSON 数字 token 精确换算成十进制整数：value = digits * 10^exp10。
// 返回 false 表示“这个数值不是整数”，或超出 long long 表示范围。全程只处理十进制字符串，
// 不经过 double，因此 9007199254740992 这种超出 IEEE-754 精确范围的值不会被舍入。
bool exact_integer_from_token(std::string_view token, long long& out) {
    bool negative = false;
    std::size_t index = 0;
    if (index < token.size() && token[index] == '-') {
        negative = true;
        ++index;
    }
    std::string digits;
    while (index < token.size() && token[index] >= '0' && token[index] <= '9') {
        digits.push_back(token[index]);
        ++index;
    }
    if (digits.empty()) {
        return false;
    }
    long long exp10 = 0;
    if (index < token.size() && token[index] == '.') {
        ++index;
        std::size_t const fraction_start = digits.size();
        while (index < token.size() && token[index] >= '0' && token[index] <= '9') {
            digits.push_back(token[index]);
            ++index;
        }
        exp10 -= static_cast<long long>(digits.size() - fraction_start);
    }
    if (index < token.size() && (token[index] == 'e' || token[index] == 'E')) {
        ++index;
        int sign = 1;
        if (index < token.size() && (token[index] == '+' || token[index] == '-')) {
            sign = token[index] == '-' ? -1 : 1;
            ++index;
        }
        if (index >= token.size() || token[index] < '0' || token[index] > '9') {
            return false;
        }
        long long exponent = 0;
        while (index < token.size() && token[index] >= '0' && token[index] <= '9') {
            exponent = exponent * 10 + (token[index] - '0');
            if (exponent > 100000) {
                return false;  // 指数离谱到不可能还是安全整数
            }
            ++index;
        }
        exp10 += sign * exponent;
    }
    if (index != token.size()) {
        return false;
    }

    // 去掉前导零：后面的长度判断才正确。
    std::size_t leading = 0;
    while (leading + 1 < digits.size() && digits[leading] == '0') {
        ++leading;
    }
    digits.erase(0, leading);
    if (digits == "0") {
        out = 0;
        return true;
    }

    if (exp10 < 0) {
        long long const drop = -exp10;
        if (drop >= static_cast<long long>(digits.size())) {
            // 需要把全部有效数字都移到小数点右边，只有当它们全是 0 时结果才是整数。
            for (char const digit : digits) {
                if (digit != '0') {
                    return false;
                }
            }
            out = 0;
            return true;
        }
        for (long long offset = 0; offset < drop; ++offset) {
            if (digits[digits.size() - 1 - static_cast<std::size_t>(offset)] != '0') {
                return false;  // 小数部分非零
            }
        }
        digits.resize(digits.size() - static_cast<std::size_t>(drop));
        leading = 0;
        while (leading + 1 < digits.size() && digits[leading] == '0') {
            ++leading;
        }
        digits.erase(0, leading);
    } else if (exp10 > 0) {
        if (static_cast<long long>(digits.size()) + exp10 > 19) {
            return false;
        }
        digits.append(static_cast<std::size_t>(exp10), '0');
    }

    if (digits.size() > 19) {
        return false;
    }
    unsigned long long magnitude = 0;
    for (char const digit : digits) {
        magnitude = magnitude * 10 + static_cast<unsigned long long>(digit - '0');
    }
    if (!negative) {
        if (magnitude > 9223372036854775807ULL) {
            return false;
        }
        out = static_cast<long long>(magnitude);
        return true;
    }
    if (magnitude > 9223372036854775808ULL) {
        return false;
    }
    out = magnitude == 9223372036854775808ULL ? LLONG_MIN : -static_cast<long long>(magnitude);
    return true;
}

class Parser {
public:
    Parser(std::string_view text, JsonParseLimits limits) : text_(text), limits_(limits) {}

    bool parse(JsonValue& out, std::string& error) {
        skip_whitespace();
        if (!parse_value(out, 1)) {
            error = error_;
            return false;
        }
        skip_whitespace();
        if (index_ != text_.size()) {
            fail(index_, "根值之后还有多余内容");
            error = error_;
            return false;
        }
        return true;
    }

private:
    bool fail(std::size_t index, char const* reason) {
        if (error_.empty()) {
            error_ = "byte " + std::to_string(index) + ": " + reason;
        }
        return false;
    }

    bool at_end() const { return index_ >= text_.size(); }

    char peek() const { return at_end() ? '\0' : text_[index_]; }

    void skip_whitespace() {
        while (!at_end()) {
            char const c = text_[index_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++index_;
            } else {
                break;
            }
        }
    }

    bool literal(char const* word) {
        std::size_t const length = std::char_traits<char>::length(word);
        if (text_.compare(index_, length, word) != 0) {
            return false;
        }
        index_ += length;
        return true;
    }

    bool parse_value(JsonValue& out, std::size_t depth) {
        if (at_end()) {
            return fail(index_, "值还没有写完");
        }
        switch (peek()) {
            case '{':
                return parse_object(out, depth);
            case '[':
                return parse_array(out, depth);
            case '"':
                out.kind = JsonValue::Kind::String;
                return parse_string(out.text);
            case 't':
                if (!literal("true")) {
                    return fail(index_, "不是合法值");
                }
                out.kind = JsonValue::Kind::Bool;
                out.boolean = true;
                return true;
            case 'f':
                if (!literal("false")) {
                    return fail(index_, "不是合法值");
                }
                out.kind = JsonValue::Kind::Bool;
                out.boolean = false;
                return true;
            case 'n':
                if (!literal("null")) {
                    return fail(index_, "不是合法值");
                }
                out.kind = JsonValue::Kind::Null;
                return true;
            default:
                return parse_number(out);
        }
    }

    bool parse_object(JsonValue& out, std::size_t depth) {
        if (depth > limits_.max_depth) {
            return fail(index_, "嵌套深度超过上限");
        }
        ++index_;  // '{'
        out.kind = JsonValue::Kind::Object;
        skip_whitespace();
        if (peek() == '}') {
            ++index_;
            return true;
        }
        // 用哈希集合查重：1 MiB 的正文里可能有上万个成员名，逐个线性扫描会退化成 O(n²)。
        std::unordered_set<std::string> names;
        for (;;) {
            skip_whitespace();
            if (peek() != '"') {
                return fail(index_, "对象的成员名必须是字符串");
            }
            std::string name;
            if (!parse_string(name)) {
                return false;
            }
            if (!names.insert(name).second) {
                return fail(index_, "对象的成员名重复");
            }
            skip_whitespace();
            if (peek() != ':') {
                return fail(index_, "成员名后面缺少冒号");
            }
            ++index_;
            skip_whitespace();
            JsonValue value;
            if (!parse_value(value, depth + 1)) {
                return false;
            }
            out.members.emplace_back(std::move(name), std::move(value));
            skip_whitespace();
            if (peek() == ',') {
                ++index_;
                continue;
            }
            if (peek() == '}') {
                ++index_;
                return true;
            }
            return fail(index_, "对象里缺少逗号或右花括号");
        }
    }

    bool parse_array(JsonValue& out, std::size_t depth) {
        if (depth > limits_.max_depth) {
            return fail(index_, "嵌套深度超过上限");
        }
        ++index_;  // '['
        out.kind = JsonValue::Kind::Array;
        skip_whitespace();
        if (peek() == ']') {
            ++index_;
            return true;
        }
        for (;;) {
            skip_whitespace();
            JsonValue value;
            if (!parse_value(value, depth + 1)) {
                return false;
            }
            out.items.push_back(std::move(value));
            skip_whitespace();
            if (peek() == ',') {
                ++index_;
                continue;
            }
            if (peek() == ']') {
                ++index_;
                return true;
            }
            return fail(index_, "数组里缺少逗号或右方括号");
        }
    }

    bool parse_string(std::string& out) {
        ++index_;  // '"'
        out.clear();
        for (;;) {
            if (at_end()) {
                return fail(index_, "字符串没有收尾的引号");
            }
            unsigned char const byte = static_cast<unsigned char>(text_[index_]);
            if (byte == '"') {
                ++index_;
                return true;
            }
            if (byte == '\\') {
                ++index_;
                if (!parse_escape(out)) {
                    return false;
                }
                continue;
            }
            if (byte < 0x20) {
                return fail(index_, "字符串里有没转义的控制字符");
            }
            // 非 ASCII 字节必须组成合法 UTF-8 序列，原样拷贝。
            if (byte < 0x80) {
                out.push_back(static_cast<char>(byte));
                ++index_;
                continue;
            }
            std::size_t const start = index_;
            std::size_t probe = index_;
            CodePoint code_point = 0;
            if (!utf8_decode(text_, probe, code_point)) {
                return fail(start, "字符串里不是合法 UTF-8");
            }
            out.append(text_.substr(start, probe - start));
            index_ = probe;
        }
    }

    static void append_code_point(std::string& out, CodePoint code_point) {
        if (code_point < 0x80) {
            out.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }

    bool parse_hex4(CodePoint& out) {
        if (index_ + 4 > text_.size()) {
            return fail(index_, "\\u 转义只有 4 位十六进制");
        }
        CodePoint value = 0;
        for (int digit = 0; digit < 4; ++digit) {
            char const c = text_[index_ + digit];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<CodePoint>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<CodePoint>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<CodePoint>(c - 'A' + 10);
            } else {
                return fail(index_ + digit, "\\u 转义里有非十六进制字符");
            }
        }
        index_ += 4;
        out = value;
        return true;
    }

    bool parse_escape(std::string& out) {
        if (at_end()) {
            return fail(index_, "反斜杠后面缺少转义字符");
        }
        char const c = text_[index_++];
        switch (c) {
            case '"': out.push_back('"'); return true;
            case '\\': out.push_back('\\'); return true;
            case '/': out.push_back('/'); return true;
            case 'b': out.push_back('\b'); return true;
            case 'f': out.push_back('\f'); return true;
            case 'n': out.push_back('\n'); return true;
            case 'r': out.push_back('\r'); return true;
            case 't': out.push_back('\t'); return true;
            case 'u': break;
            default:
                return fail(index_ - 1, "不认识的转义字符");
        }

        CodePoint high = 0;
        if (!parse_hex4(high)) {
            return false;
        }
        if (high >= 0xD800 && high <= 0xDBFF) {
            if (index_ + 1 >= text_.size() || text_[index_] != '\\' || text_[index_ + 1] != 'u') {
                return fail(index_, "高代理项后面没有配对的 \\u 转义");
            }
            index_ += 2;
            CodePoint low = 0;
            if (!parse_hex4(low)) {
                return false;
            }
            if (low < 0xDC00 || low > 0xDFFF) {
                return fail(index_, "低代理项不在 D C 00..DFFF 之内");
            }
            append_code_point(out, 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00));
            return true;
        }
        if (high >= 0xDC00 && high <= 0xDFFF) {
            return fail(index_, "孤立的低代理项");
        }
        append_code_point(out, high);
        return true;
    }

    bool parse_number(JsonValue& out) {
        std::size_t const start = index_;
        if (peek() == '-') {
            ++index_;
        }
        if (at_end() || peek() < '0' || peek() > '9') {
            return fail(start, "不是合法值");
        }
        if (peek() == '0') {
            ++index_;
        } else {
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                ++index_;
            }
        }
        if (!at_end() && peek() == '.') {
            ++index_;
            if (at_end() || peek() < '0' || peek() > '9') {
                return fail(index_, "小数点后面没有数字");
            }
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                ++index_;
            }
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            ++index_;
            if (!at_end() && (peek() == '+' || peek() == '-')) {
                ++index_;
            }
            if (at_end() || peek() < '0' || peek() > '9') {
                return fail(index_, "指数后面没有数字");
            }
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                ++index_;
            }
        }

        std::string const token(text_.substr(start, index_ - start));
        out.kind = JsonValue::Kind::Number;
        out.number_token = token;
        out.number = std::strtod(token.c_str(), nullptr);
        out.is_integer = false;
        out.integer = 0;
        long long exact = 0;
        if (json_number_as_integer(out, exact)) {
            out.is_integer = true;
            out.integer = exact;
        }
        return true;
    }

    std::string_view text_;
    std::size_t index_ = 0;
    JsonParseLimits limits_;
    std::string error_;
};

}  // namespace

bool json_parse(std::string_view utf8, JsonValue& out, std::string& error, JsonParseLimits limits) {
    out = JsonValue{};
    error.clear();
    if (!utf8_validate(utf8)) {
        error = "byte 0: 正文不是合法 UTF-8";
        return false;
    }
    Parser parser(utf8, limits);
    return parser.parse(out, error);
}

bool json_number_as_integer(JsonValue const& value, long long& out) {
    if (!value.is_number()) {
        return false;
    }
    return exact_integer_from_token(value.number_token, out);
}

}  // namespace deskfeed
