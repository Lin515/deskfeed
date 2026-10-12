#include "ipc/control_wire.h"

#include "core/utf8.h"

// 发送端只需要转换（`utf8_from_wide`），不需要 `utf8_validate`：这一半会被编译进任务栏负载
// DLL，所以它只链 `deskfeed_core_text`。详见 `control_wire.h` 的文件头。
namespace deskfeed {
namespace {

void put_u32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

}  // namespace

std::string control_encode(ControlRequest const& request) {
    std::string out;
    put_u32(out, kControlMagic);
    put_u32(out, kControlVersion);
    put_u32(out, static_cast<std::uint32_t>(request.op));
    put_u32(out, request.sender_pid);
    put_u32(out, static_cast<std::uint32_t>(request.args.size()));
    put_u32(out, 0);  // 预留
    for (std::wstring const& arg : request.args) {
        std::string const bytes = utf8_from_wide(arg);
        put_u32(out, static_cast<std::uint32_t>(bytes.size()));
        out.append(bytes);
    }
    return out;
}

}  // namespace deskfeed
