#include "core/snapshot.h"

#include "core/json.h"
#include "core/utf8.h"

#include <utility>

namespace deskfeed {
namespace {

ValidationResult fail(ValidationKind kind, std::string message) {
    ValidationResult result;
    result.ok = false;
    result.kind = kind;
    result.message = std::move(message);
    return result;
}

ValidationResult pass() {
    ValidationResult result;
    result.ok = true;
    result.kind = ValidationKind::None;
    return result;
}

std::string item_prefix(std::size_t index) { return "items[" + std::to_string(index) + "]"; }

// 可选字段：允许省略，但不允许用 null 代替。
bool optional_string(JsonValue const& object, char const* name, bool* present, std::string* out,
                     ValidationKind* kind, std::string* message) {
    JsonValue const* member = object.member(name);
    if (member == nullptr) {
        *present = false;
        out->clear();
        return true;
    }
    if (member->is_null()) {
        *kind = ValidationKind::FieldType;
        *message = std::string(name) + " 不能用 null";
        return false;
    }
    if (!member->is_string()) {
        *kind = ValidationKind::FieldType;
        *message = std::string(name) + " 必须是字符串";
        return false;
    }
    *present = true;
    *out = member->text;
    return true;
}

}  // namespace

char const* item_state_name(ItemState state) {
    switch (state) {
        case ItemState::Ok: return "ok";
        case ItemState::Warn: return "warn";
        case ItemState::Error: return "error";
    }
    return "ok";
}

char const* validation_kind_name(ValidationKind kind) {
    switch (kind) {
        case ValidationKind::None: return "none";
        case ValidationKind::TooLarge: return "too-large";
        case ValidationKind::Encoding: return "encoding";
        case ValidationKind::Parse: return "parse";
        case ValidationKind::Root: return "root";
        case ValidationKind::Version: return "version";
        case ValidationKind::Items: return "items";
        case ValidationKind::ItemShape: return "item-shape";
        case ValidationKind::RequiredField: return "required-field";
        case ValidationKind::FieldType: return "field-type";
        case ValidationKind::FieldLimit: return "field-limit";
        case ValidationKind::ItemCount: return "item-count";
        case ValidationKind::DuplicateId: return "duplicate-id";
        case ValidationKind::EmptyId: return "empty-id";
        case ValidationKind::InvalidState: return "invalid-state";
        case ValidationKind::Blank: return "blank";
        case ValidationKind::SingleLine: return "single-line";
        case ValidationKind::Expiry: return "expiry";
    }
    return "unknown";
}

ValidationResult snapshot_validate(std::string_view body, Snapshot& out) {
    if (body.size() > kMaxBodyBytes) {
        return fail(ValidationKind::TooLarge,
                    "正文 " + std::to_string(body.size()) + " 字节，超过 " +
                        std::to_string(kMaxBodyBytes) + " 字节上限");
    }

    // 允许开头的 UTF-8 BOM（EF BB BF），其余位置出现 BOM 一律当普通字符交给 JSON 解析。
    std::string_view text = body;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }

    if (!utf8_validate(text)) {
        return fail(ValidationKind::Encoding, "正文不是合法 UTF-8");
    }

    JsonValue root;
    std::string parse_error;
    if (!json_parse(text, root, parse_error, JsonParseLimits{kMaxJsonDepth})) {
        return fail(ValidationKind::Parse, parse_error);
    }
    if (!root.is_object()) {
        return fail(ValidationKind::Root, "根值必须是对象");
    }

    Snapshot snapshot;

    JsonValue const* version = root.member("version");
    if (version == nullptr) {
        return fail(ValidationKind::Version, "缺少 version");
    }
    if (!version->is_number()) {
        return fail(ValidationKind::Version, "version 必须是数字 1");
    }
    if (version->number != 1.0) {
        return fail(ValidationKind::Version, "不支持的 version（只接受 1）");
    }
    snapshot.version = 1;

    JsonValue const* items = root.member("items");
    if (items == nullptr) {
        return fail(ValidationKind::Items, "缺少 items");
    }
    if (!items->is_array()) {
        return fail(ValidationKind::Items, "items 必须是数组");
    }
    if (items->items.size() > kMaxItems) {
        return fail(ValidationKind::ItemCount,
                    "items 有 " + std::to_string(items->items.size()) + " 项，超过 " +
                        std::to_string(kMaxItems) + " 项上限");
    }

    if (JsonValue const* expires = root.member("expiresAtMs"); expires != nullptr) {
        if (expires->is_null()) {
            return fail(ValidationKind::Expiry, "expiresAtMs 不能用 null");
        }
        // 按“数值”判断，不按写法：1000.0、1e3、9.007199254740991e15 都是合法整数，
        // 1e-3、1.5 以及超出安全整数范围的值整份拒绝。
        long long value = 0;
        if (!expires->is_number() || !json_number_as_integer(*expires, value)) {
            return fail(ValidationKind::Expiry,
                        "expiresAtMs 必须是非负安全整数（Unix 毫秒），当前不是一个整数");
        }
        if (value < 0 || value > kMaxSafeInteger) {
            return fail(ValidationKind::Expiry,
                        "expiresAtMs 超出 0.." + std::to_string(kMaxSafeInteger));
        }
        snapshot.has_expires_at = true;
        snapshot.expires_at_ms = value;
    }

    snapshot.items.reserve(items->items.size());
    for (std::size_t index = 0; index < items->items.size(); ++index) {
        JsonValue const& element = items->items[index];
        std::string const prefix = item_prefix(index);
        if (!element.is_object()) {
            return fail(ValidationKind::ItemShape, prefix + " 必须是对象");
        }

        SnapshotItem item;

        JsonValue const* id = element.member("id");
        if (id == nullptr) {
            return fail(ValidationKind::RequiredField, prefix + " 缺少 id");
        }
        if (!id->is_string()) {
            return fail(ValidationKind::FieldType, prefix + ".id 必须是字符串");
        }
        if (id->text.empty()) {
            return fail(ValidationKind::EmptyId, prefix + ".id 不能是空字符串");
        }
        if (id->text.size() > kMaxIdBytes) {
            return fail(ValidationKind::FieldLimit,
                        prefix + ".id 有 " + std::to_string(id->text.size()) + " 字节，超过 " +
                            std::to_string(kMaxIdBytes));
        }
        for (SnapshotItem const& existing : snapshot.items) {
            if (existing.id == id->text) {
                return fail(ValidationKind::DuplicateId, prefix + ".id 与前面的条目重复：" + id->text);
            }
        }
        item.id = id->text;

        JsonValue const* label = element.member("label");
        if (label == nullptr) {
            return fail(ValidationKind::RequiredField, prefix + " 缺少 label");
        }
        if (!label->is_string()) {
            return fail(ValidationKind::FieldType, prefix + ".label 必须是字符串");
        }
        if (label->text.size() > kMaxLabelBytes) {
            return fail(ValidationKind::FieldLimit,
                        prefix + ".label 有 " + std::to_string(label->text.size()) + " 字节，超过 " +
                            std::to_string(kMaxLabelBytes));
        }
        if (has_line_break(label->text)) {
            return fail(ValidationKind::SingleLine, prefix + ".label 必须是单行字符串");
        }
        item.label = label->text;

        JsonValue const* value = element.member("value");
        if (value == nullptr) {
            return fail(ValidationKind::RequiredField, prefix + " 缺少 value");
        }
        if (!value->is_string()) {
            return fail(ValidationKind::FieldType, prefix + ".value 必须是字符串");
        }
        if (value->text.size() > kMaxValueBytes) {
            return fail(ValidationKind::FieldLimit,
                        prefix + ".value 有 " + std::to_string(value->text.size()) + " 字节，超过 " +
                            std::to_string(kMaxValueBytes));
        }
        if (has_line_break(value->text)) {
            return fail(ValidationKind::SingleLine, prefix + ".value 必须是单行字符串");
        }
        item.value = value->text;

        if (is_blank(item.label) && is_blank(item.value)) {
            return fail(ValidationKind::Blank, prefix + " 的 label 与 value 不能同时为空白");
        }

        bool state_present = false;
        std::string state_text;
        ValidationKind kind = ValidationKind::None;
        std::string message;
        if (!optional_string(element, "state", &state_present, &state_text, &kind, &message)) {
            return fail(kind, prefix + "." + message);
        }
        if (state_present) {
            if (state_text == "ok") {
                item.state = ItemState::Ok;
            } else if (state_text == "warn") {
                item.state = ItemState::Warn;
            } else if (state_text == "error") {
                item.state = ItemState::Error;
            } else {
                return fail(ValidationKind::InvalidState,
                            prefix + ".state 只允许 ok / warn / error，收到 " + state_text);
            }
        }

        bool detail_present = false;
        std::string detail_text;
        if (!optional_string(element, "detail", &detail_present, &detail_text, &kind, &message)) {
            return fail(kind, prefix + "." + message);
        }
        if (detail_present) {
            if (detail_text.size() > kMaxDetailBytes) {
                return fail(ValidationKind::FieldLimit,
                            prefix + ".detail 有 " + std::to_string(detail_text.size()) +
                                " 字节，超过 " + std::to_string(kMaxDetailBytes));
            }
            item.detail = detail_text;
        }

        snapshot.items.push_back(std::move(item));
    }

    out = std::move(snapshot);
    return pass();
}

}  // namespace deskfeed
