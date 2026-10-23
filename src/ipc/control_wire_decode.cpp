#include "ipc/control_wire.h"

#include "core/utf8.h"

// 接收端：解码 + 校验。这是主进程那一半，不进任务栏负载 DLL，所以它可以随意用 core 的
// UTF-8 校验器。线格式的字段顺序与 `control_wire_encode.cpp` 必须逐字节一致。
namespace deskfeed {
namespace {

bool take_u32(std::string_view bytes, std::size_t& offset, std::uint32_t& value) {
    if (offset + 4 > bytes.size()) {
        return false;
    }
    value = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
    offset += 4;
    return true;
}

}  // namespace

std::intptr_t control_reply_value(ControlReply reply) { return static_cast<std::intptr_t>(reply); }

bool control_decode(std::string_view bytes, ControlRequest& out, std::string& error) {
    if (bytes.size() > kControlMaxPayloadBytes) {
        error = "control payload too large";
        return false;
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t op = 0;
    std::uint32_t sender = 0;
    std::uint32_t count = 0;
    std::uint32_t reserved = 0;
    if (!take_u32(bytes, offset, magic) || !take_u32(bytes, offset, version) ||
        !take_u32(bytes, offset, op) || !take_u32(bytes, offset, sender) ||
        !take_u32(bytes, offset, count) || !take_u32(bytes, offset, reserved)) {
        error = "control payload truncated";
        return false;
    }
    if (magic != kControlMagic) {
        error = "control magic mismatch";
        return false;
    }
    if (version != kControlVersion) {
        error = "control version mismatch";
        return false;
    }
    if (op != static_cast<std::uint32_t>(ControlOp::Command) &&
        op != static_cast<std::uint32_t>(ControlOp::Inspect) &&
        op != static_cast<std::uint32_t>(ControlOp::Refresh) &&
        op != static_cast<std::uint32_t>(ControlOp::ShowWidget) &&
        op != static_cast<std::uint32_t>(ControlOp::SetScreens)) {
        error = "control op unknown";
        return false;
    }
    if (count > kControlMaxArgs) {
        error = "control argument count out of range";
        return false;
    }

    ControlRequest request;
    request.op = static_cast<ControlOp>(op);
    request.sender_pid = sender;
    request.args.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t length = 0;
        if (!take_u32(bytes, offset, length)) {
            error = "control argument truncated";
            return false;
        }
        if (length > kControlMaxArgBytes || offset + length > bytes.size()) {
            error = "control argument length out of range";
            return false;
        }
        std::string_view const text = bytes.substr(offset, length);
        offset += length;
        if (!utf8_validate(text)) {
            error = "control argument is not valid UTF-8";
            return false;
        }
        request.args.push_back(wide_from_utf8(text));
    }
    if (offset != bytes.size()) {
        error = "control payload has trailing bytes";
        return false;
    }
    out = std::move(request);
    return true;
}

}  // namespace deskfeed
