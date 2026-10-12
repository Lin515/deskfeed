#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// 快照协议 v1：模型与严格校验。
//
// 这一层是纯逻辑：输入是响应正文的字节，输出是快照或一条拒绝原因；不联网、不碰窗口、
// 不读写磁盘。生产者、检查器、主进程的取数线程共用同一份校验，因此“什么算合法快照”
// 在公开工程里只有一处定义。
//
// 完整规范见 docs/protocol.md，机器可读的约束见 schema/snapshot-v1.schema.json。
// 本文件顶部的常量就是容量边界的唯一出处（schema 与文档必须与它一致）。
namespace deskfeed {

// 容量边界（见 docs/protocol.md 的“容量边界”）。
inline constexpr std::size_t kMaxBodyBytes = 1024 * 1024;  // 1 MiB
inline constexpr std::size_t kMaxItems = 64;
inline constexpr std::size_t kMaxIdBytes = 128;
inline constexpr std::size_t kMaxLabelBytes = 256;
inline constexpr std::size_t kMaxValueBytes = 1024;
inline constexpr std::size_t kMaxDetailBytes = 16384;
inline constexpr std::size_t kMaxJsonDepth = 32;
inline constexpr long long kMaxSafeInteger = 9007199254740991LL;  // 2^53 - 1

enum class ItemState { Ok, Warn, Error };

char const* item_state_name(ItemState state);

// items[] 里的一项。文本是纯文本，不是 HTML / Markdown / XAML / 格式化指令。
struct SnapshotItem {
    std::string id;
    std::string label;
    std::string value;
    ItemState state = ItemState::Ok;
    std::string detail;
};

struct Snapshot {
    long long version = 0;
    std::vector<SnapshotItem> items;
    bool has_expires_at = false;
    long long expires_at_ms = 0;  // Unix 毫秒

    bool expired_at(long long now_ms) const {
        return has_expires_at && now_ms >= expires_at_ms;
    }
};

// 拒绝原因的分类。展示层只关心“结构错误”与“业务 error”不同：
// 校验失败整份拒绝、没有条目可显示；而某一条的 state=error 是业务状态，不影响其他条目。
enum class ValidationKind {
    None,
    TooLarge,       // 正文超过 kMaxBodyBytes
    Encoding,       // 正文不是合法 UTF-8
    Parse,          // 不是合法 JSON
    Root,           // 根值不是对象
    Version,        // version 缺失、类型不对或不等于 1
    Items,          // items 缺失或不是数组
    ItemShape,      // items[] 的元素不是对象
    RequiredField,  // id / label / value 缺失
    FieldType,      // 已知字段类型不对（含用 null 代替可选字段）
    FieldLimit,     // 字符串超过对应字节上限
    ItemCount,      // 条目数超过 kMaxItems
    DuplicateId,    // 同一快照内 id 重复
    EmptyId,        // id 是空字符串
    InvalidState,   // state 不在 ok / warn / error 之内
    Blank,          // label 与 value 同时为空白
    SingleLine,     // label / value 含换行
    Expiry,         // expiresAtMs 不是非负安全整数
};

struct ValidationResult {
    bool ok = false;
    ValidationKind kind = ValidationKind::None;
    std::string message;  // 面向排障的一行说明（UTF-8）
};

// 正文 -> 快照。允许开头的 UTF-8 BOM；失败时 out 不被使用。
ValidationResult snapshot_validate(std::string_view body, Snapshot& out);

// 便于日志与检查器统一措辞。
char const* validation_kind_name(ValidationKind kind);

}  // namespace deskfeed
