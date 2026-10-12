#include "display/frame.h"

// 线格式（全部小端、定长）：
//
//   前导：
//     u32 item_count
//     u32 kind            （0 waiting / 1 content / 2 error）
//     u32 alarm           （0 / 1）
//     u32 failure         （FailureKind 的稳定编号）
//     u32 reason_bytes
//     reason 的 UTF-8 字节
//   之后每条 item 一条记录：
//     u32 state           （0 ok / 1 warn / 2 error）
//     u32 id_bytes
//     u32 label_bytes
//     u32 value_bytes
//     u32 detail_bytes
//     id / label / value / detail 的 UTF-8 字节（按上面的顺序，无填充）
//
// 解码端逐项做边界检查：长度超过协议上限、枚举越界、游标越过 payload_bytes 都整帧拒绝。
// 这一层只处理字节，不碰共享内存，所以可以在普通用例里离线测。

namespace deskfeed {
namespace {

void put_u32(std::vector<unsigned char>& out, std::uint32_t value) {
    out.push_back(static_cast<unsigned char>(value & 0xFFu));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFFu));
    out.push_back(static_cast<unsigned char>((value >> 16) & 0xFFu));
    out.push_back(static_cast<unsigned char>((value >> 24) & 0xFFu));
}

void put_bytes(std::vector<unsigned char>& out, std::string const& text) {
    out.insert(out.end(), text.begin(), text.end());
}

bool u32_of_state(ItemState state, std::uint32_t& out) {
    switch (state) {
        case ItemState::Ok: out = 0; return true;
        case ItemState::Warn: out = 1; return true;
        case ItemState::Error: out = 2; return true;
    }
    return false;
}

bool u32_of_kind(ViewKind kind, std::uint32_t& out) {
    switch (kind) {
        case ViewKind::Waiting: out = 0; return true;
        case ViewKind::Content: out = 1; return true;
        case ViewKind::Error: out = 2; return true;
    }
    return false;
}

bool u32_of_failure(FailureKind failure, std::uint32_t& out) {
    switch (failure) {
        case FailureKind::None: out = 0; return true;
        case FailureKind::NoUrl: out = 1; return true;
        case FailureKind::Transport: out = 2; return true;
        case FailureKind::Timeout: out = 3; return true;
        case FailureKind::HttpStatus: out = 4; return true;
        case FailureKind::Redirect: out = 5; return true;
        case FailureKind::TooLarge: out = 6; return true;
        case FailureKind::Encoding: out = 7; return true;
        case FailureKind::Parse: out = 8; return true;
        case FailureKind::Validation: out = 9; return true;
        case FailureKind::Internal: out = 10; return true;
    }
    return false;
}

bool state_of_u32(std::uint32_t value, ItemState& out) {
    switch (value) {
        case 0: out = ItemState::Ok; return true;
        case 1: out = ItemState::Warn; return true;
        case 2: out = ItemState::Error; return true;
        default: return false;
    }
}

bool kind_of_u32(std::uint32_t value, ViewKind& out) {
    switch (value) {
        case 0: out = ViewKind::Waiting; return true;
        case 1: out = ViewKind::Content; return true;
        case 2: out = ViewKind::Error; return true;
        default: return false;
    }
}

bool failure_of_u32(std::uint32_t value, FailureKind& out) {
    switch (value) {
        case 0: out = FailureKind::None; return true;
        case 1: out = FailureKind::NoUrl; return true;
        case 2: out = FailureKind::Transport; return true;
        case 3: out = FailureKind::Timeout; return true;
        case 4: out = FailureKind::HttpStatus; return true;
        case 5: out = FailureKind::Redirect; return true;
        case 6: out = FailureKind::TooLarge; return true;
        case 7: out = FailureKind::Encoding; return true;
        case 8: out = FailureKind::Parse; return true;
        case 9: out = FailureKind::Validation; return true;
        case 10: out = FailureKind::Internal; return true;
        default: return false;
    }
}

// 只读游标：每一步都检查剩余长度，越界即失败，绝不越过 payload_bytes。
struct Cursor {
    unsigned char const* data = nullptr;
    std::size_t size = 0;
    std::size_t offset = 0;

    bool read_u32(std::uint32_t& out) {
        if (size - offset < sizeof(std::uint32_t)) {
            return false;
        }
        out = static_cast<std::uint32_t>(data[offset]) |
              (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
              (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
              (static_cast<std::uint32_t>(data[offset + 3]) << 24);
        offset += sizeof(std::uint32_t);
        return true;
    }

    bool read_text(std::size_t count, std::size_t limit, std::string& out) {
        if (count > limit || size - offset < count) {
            return false;
        }
        out.assign(reinterpret_cast<char const*>(data + offset), count);
        offset += count;
        return true;
    }
};

}  // namespace

Frame frame_from_view(View const& view) {
    Frame frame;
    frame.kind = view.kind;
    frame.alarm = view.alarm;
    frame.failure = view.failure;
    frame.reason = view.reason;
    frame.items.reserve(view.items.size());
    for (ViewItem const& item : view.items) {
        FrameItem out;
        out.id = item.id;
        out.label = item.label;
        out.value = item.value;
        out.state = item.state;
        out.detail = item.detail;
        frame.items.push_back(std::move(out));
    }
    return frame;
}

bool frame_encode(Frame const& frame, std::vector<unsigned char>& out) {
    if (frame.items.size() > kMaxItems || frame.reason.size() > kFrameMaxReasonBytes) {
        return false;
    }
    std::uint32_t kind = 0;
    std::uint32_t failure = 0;
    if (!u32_of_kind(frame.kind, kind) || !u32_of_failure(frame.failure, failure)) {
        return false;
    }

    out.clear();
    out.reserve(256);
    put_u32(out, static_cast<std::uint32_t>(frame.items.size()));
    put_u32(out, kind);
    put_u32(out, frame.alarm ? 1u : 0u);
    put_u32(out, failure);
    put_u32(out, static_cast<std::uint32_t>(frame.reason.size()));
    put_bytes(out, frame.reason);

    for (FrameItem const& item : frame.items) {
        if (item.id.size() > kMaxIdBytes || item.label.size() > kMaxLabelBytes ||
            item.value.size() > kMaxValueBytes || item.detail.size() > kMaxDetailBytes) {
            out.clear();
            return false;
        }
        std::uint32_t state = 0;
        if (!u32_of_state(item.state, state)) {
            out.clear();
            return false;
        }
        put_u32(out, state);
        put_u32(out, static_cast<std::uint32_t>(item.id.size()));
        put_u32(out, static_cast<std::uint32_t>(item.label.size()));
        put_u32(out, static_cast<std::uint32_t>(item.value.size()));
        put_u32(out, static_cast<std::uint32_t>(item.detail.size()));
        put_bytes(out, item.id);
        put_bytes(out, item.label);
        put_bytes(out, item.value);
        put_bytes(out, item.detail);
    }

    if (out.size() > kFramePayloadCapacity) {
        out.clear();
        return false;
    }
    return true;
}

bool frame_decode(unsigned char const* payload, std::size_t payload_bytes, Frame& out) {
    if (payload == nullptr) {
        return false;
    }
    Cursor cursor{payload, payload_bytes, 0};

    std::uint32_t item_count = 0;
    std::uint32_t kind = 0;
    std::uint32_t alarm = 0;
    std::uint32_t failure = 0;
    std::uint32_t reason_bytes = 0;
    if (!cursor.read_u32(item_count) || !cursor.read_u32(kind) || !cursor.read_u32(alarm) ||
        !cursor.read_u32(failure) || !cursor.read_u32(reason_bytes)) {
        return false;
    }
    if (item_count > kMaxItems || alarm > 1) {
        return false;
    }
    Frame decoded;
    if (!kind_of_u32(kind, decoded.kind) || !failure_of_u32(failure, decoded.failure)) {
        return false;
    }
    if (!cursor.read_text(reason_bytes, kFrameMaxReasonBytes, decoded.reason)) {
        return false;
    }
    decoded.alarm = alarm != 0;

    decoded.items.reserve(item_count);
    for (std::uint32_t index = 0; index < item_count; ++index) {
        std::uint32_t state = 0;
        std::uint32_t id_bytes = 0;
        std::uint32_t label_bytes = 0;
        std::uint32_t value_bytes = 0;
        std::uint32_t detail_bytes = 0;
        if (!cursor.read_u32(state) || !cursor.read_u32(id_bytes) || !cursor.read_u32(label_bytes) ||
            !cursor.read_u32(value_bytes) || !cursor.read_u32(detail_bytes)) {
            return false;
        }
        FrameItem item;
        if (!state_of_u32(state, item.state)) {
            return false;
        }
        if (!cursor.read_text(id_bytes, kMaxIdBytes, item.id) ||
            !cursor.read_text(label_bytes, kMaxLabelBytes, item.label) ||
            !cursor.read_text(value_bytes, kMaxValueBytes, item.value) ||
            !cursor.read_text(detail_bytes, kMaxDetailBytes, item.detail)) {
            return false;
        }
        decoded.items.push_back(std::move(item));
    }

    // 载荷必须被完整消费；尾巴上多出来的字节说明双方对格式的理解已经不一致。
    if (cursor.offset != payload_bytes) {
        return false;
    }
    out = std::move(decoded);
    return true;
}

char const* frame_view_kind_name(ViewKind kind) {
    switch (kind) {
        case ViewKind::Waiting: return "waiting";
        case ViewKind::Content: return "content";
        case ViewKind::Error: return "error";
    }
    return "unknown";
}

char const* frame_failure_name(FailureKind failure) {
    switch (failure) {
        case FailureKind::None: return "none";
        case FailureKind::NoUrl: return "no-url";
        case FailureKind::Transport: return "transport";
        case FailureKind::Timeout: return "timeout";
        case FailureKind::HttpStatus: return "http-status";
        case FailureKind::Redirect: return "redirect";
        case FailureKind::TooLarge: return "too-large";
        case FailureKind::Encoding: return "encoding";
        case FailureKind::Parse: return "parse";
        case FailureKind::Validation: return "validation";
        case FailureKind::Internal: return "internal";
    }
    return "unknown";
}

char const* frame_item_state_name(ItemState state) {
    switch (state) {
        case ItemState::Ok: return "ok";
        case ItemState::Warn: return "warn";
        case ItemState::Error: return "error";
    }
    return "unknown";
}

}  // namespace deskfeed
