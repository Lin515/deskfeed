#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 极简只读 JSON 解析器（RFC 8259），专供快照协议使用。
//
// 引入它的理由：它读的 JSON 形状由本协议自身规定，为它拉一个第三方库、或引入
// Windows.Data.Json 那层 WinRT 依赖都不划算。它**只跑在主进程里**：任务栏负载 DLL 读的是
// 主进程已经算好的显示帧，自己不做任何 JSON 解析（见 docs/architecture.md 的 seam 表）。
// 这里只实现取值需要的那部分：对象、数组、字符串（含 \uXXXX 与代理对）、数字、布尔与 null。
//
// 与“宽容解析”的差别（协议要求严格校验，见 docs/protocol.md）：
//   * 只接受合法 UTF-8，字符串里的裸控制字符按 RFC 拒绝；
//   * 同一个对象里重名的成员整份拒绝（对象成员按出现顺序保存）；
//   * 根值解析完之后还有非空白内容（如两个 JSON 值拼接）整份拒绝；
//   * 嵌套深度的计数方式：根对象/数组算第 1 层，超过 limits.max_depth 拒绝。
//
// 解析失败不抛异常：返回 false，并把“字节下标 + 原因”写进 error。
namespace deskfeed {

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    // 数字 token 的原始写法（例如 "1e3"、"-0"、"9007199254740991.0"）。
    // 需要按“数值”而不是按写法判断是不是安全整数时用它做精确换算。
    std::string number_token;
    // 仅当 token 本身没有小数点与指数、且落在 long long 范围内时为 true。
    bool is_integer = false;
    long long integer = 0;
    std::string text;                                       // String 的值（已解码的 UTF-8）
    std::vector<JsonValue> items;                           // Array
    std::vector<std::pair<std::string, JsonValue>> members;  // Object，按出现顺序且名字唯一

    bool is_object() const { return kind == Kind::Object; }
    bool is_array() const { return kind == Kind::Array; }
    bool is_string() const { return kind == Kind::String; }
    bool is_number() const { return kind == Kind::Number; }
    bool is_bool() const { return kind == Kind::Bool; }
    bool is_null() const { return kind == Kind::Null; }

    // 成员查找；不存在时返回 nullptr。返回值指针，是为了让调用方区分
    // “没有这一项”与“这一项存在但是 null”。
    JsonValue const* member(std::string_view name) const;
};

struct JsonParseLimits {
    std::size_t max_depth = 32;
};

// utf8 -> JsonValue。失败时返回 false，error 是“字节下标 + 原因”。
bool json_parse(std::string_view utf8, JsonValue& out, std::string& error,
                JsonParseLimits limits = JsonParseLimits{});

// 按数值精确判断一个数字是不是整数，并给出它的整数值。
//
// 不是看“写法”：1e3、1000.0、9.007199254740991e15 都是整数（依次为 1000、
// 9007199254740991），而 1e-3、1.5 不是。换算全程用十进制字符串，不经过 double，
// 因此不会把不安全的大整数悄悄舍入成另一个数；超出 long long 表示范围时返回 false。
bool json_number_as_integer(JsonValue const& value, long long& out);

}  // namespace deskfeed
