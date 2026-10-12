#include "inject/payload_status.h"

#include "display/frame.h"
#include "display/mailbox.h"
#include "inject/pe_export.h"
#include "platform/win32_util.h"

#include <cstring>

namespace deskfeed::inject {
namespace {

// 有界地把帧头抄一份。写端用 seqlock（奇数 = 正在写），所以这里比前后两次序号：
// 不相等就重来，超过次数就报告"不稳定"，绝不拿半张帧头当结论。
//
// 显式加 MemoryBarrier：与 `src/display/mailbox.cpp` 的读端同一套做法——只靠 volatile 读不足
// 以保证 memcpy 不会与序号读/写重排。
bool copy_stable_header(void const* view, deskfeed::FrameSharedHeader& out, std::wstring& error) {
    auto const* header = static_cast<deskfeed::FrameSharedHeader const*>(view);
    auto const* sequence = reinterpret_cast<std::uint32_t const volatile*>(&header->sequence);

    for (int attempt = 0; attempt < 32; ++attempt) {
        std::uint32_t const before = *sequence;
        if ((before & 1u) != 0) {
            ::Sleep(0);
            continue;
        }
        ::MemoryBarrier();
        std::memcpy(&out, header, sizeof(out));
        ::MemoryBarrier();
        std::uint32_t const after = *sequence;
        if (before == after) {
            return true;
        }
    }
    error = L"the display frame header never settled (a producer is writing it continuously)";
    return false;
}

// scope 的**白名单**：规范里 scope 最多 32 个字符（与 `resolve_scope` 一致）；容量 36 只是
// "装得下"的余量，不是合法长度。
inline constexpr std::size_t kPayloadScopeMaxChars = 32;

// 只读地把 POD 里的 scope 取成字符串，并同时完成全部合法性检查。校验与取值共用这一份实现，
// 免得"检查过的"和"用出去的"是两次不同的截断：负载若写了 33..35 个字符、或在容量内没有结尾，
// 都是规范之外的东西 —— **拒绝**，绝不用 `pod_scope_length()` 的截断结果冒充合法 scope。
bool pod_scope_text(PayloadStatusPod const& pod, std::wstring& out, std::wstring& why) {
    std::size_t length = 0;
    while (length < kPayloadScopeCapacity && pod.scope[length] != L'\0') {
        wchar_t const character = pod.scope[length];
        bool const allowed = (character >= L'A' && character <= L'Z') ||
                             (character >= L'a' && character <= L'z') ||
                             (character >= L'0' && character <= L'9') || character == L'_' ||
                             character == L'-';
        if (!allowed) {
            why = L"the scope string contains a character outside the allowed set";
            return false;
        }
        ++length;
    }
    if (length >= kPayloadScopeCapacity) {
        why = L"the scope string is not NUL terminated inside the ABI capacity";
        return false;
    }
    if (length > kPayloadScopeMaxChars) {
        why = L"the scope string is longer than the 32-character whitelist";
        return false;
    }
    out.assign(pod.scope, length);
    return true;
}

// POD 里的布尔字段只能是 0 或 1：别的值说明这份 POD 不是我们认识的那一份（或已经被写坏）。
bool is_flag(std::uint32_t value) { return value == 0 || value == 1; }

// 校验负载写回来的 POD：`size`/`abi` 之外，还要看每一个字段的取值域与 scope 串本身。
// 通过时把 scope 文本一并交出来（校验与取值同一份实现）。
bool pod_is_well_formed(PayloadStatusPod const& pod, std::wstring& scope_text, std::wstring& why) {
    if (pod.size != sizeof(PayloadStatusPod) || pod.abi != kPayloadStatusAbi) {
        why = L"size/abi mismatch";
        return false;
    }
    std::uint32_t const flags[] = {pod.started,   pod.thread_alive,       pod.unload_safe,
                                  pod.dangling,  pod.mailbox_attached,   pod.producer_alive,
                                  pod.xaml_block_present};
    for (std::uint32_t const flag : flags) {
        if (!is_flag(flag)) {
            why = L"a boolean field carries a value other than 0/1";
            return false;
        }
    }
    if (pod.producer_pid_high != 0) {
        // 本工具的 PID 都是 32 位；高位非 0 说明写回来的不是我们认识的字段布局。
        why = L"producer_pid_high is not zero";
        return false;
    }
    if (pod.width != kPayloadStatusWidth) {
        // `width` 是把 ABI 尺寸钉死用的显式字段：不是 64 说明这份 POD 不是我们认识的那一份。
        why = L"width is not the 64 this ABI requires";
        return false;
    }
    return pod_scope_text(pod, scope_text, why);
}

}  // namespace

char const* payload_state_name(PayloadState state) {
    switch (state) {
        case PayloadState::NotMapped: return "not-mapped";
        case PayloadState::MappedUnknownAbi: return "mapped-unknown-abi";
        case PayloadState::MappedUnverifiable: return "mapped-unverifiable";
        case PayloadState::MappedInactive: return "mapped-inactive";
        case PayloadState::MappedActive: return "mapped-active";
        case PayloadState::MappedRetained: return "mapped-retained";
    }
    return "unknown";
}

bool probe_mailbox_producer(std::wstring const& scope, MailboxProducer& out, std::wstring& error) {
    out = MailboxProducer{};
    std::wstring const name = deskfeed::display_frame_map_name(scope);
    HANDLE mapping = ::OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (mapping == nullptr) {
        // 对象不存在 = 主进程还没建邮箱（或已经收尾）。这不是错误，调用方只看 present。
        DWORD const code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        error = L"OpenFileMapping(" + name + L") failed: " + deskfeed::last_error_text(code);
        return false;
    }

    void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(deskfeed::FrameSharedHeader));
    if (view == nullptr) {
        error = L"MapViewOfFile failed: " + deskfeed::last_error_text();
        ::CloseHandle(mapping);
        return false;
    }

    deskfeed::FrameSharedHeader header{};
    bool const stable = copy_stable_header(view, header, error);
    ::UnmapViewOfFile(view);
    ::CloseHandle(mapping);
    if (!stable) {
        return false;
    }

    if (header.magic != deskfeed::kFrameMagic || header.version != deskfeed::kFrameVersion ||
        header.header_bytes != sizeof(deskfeed::FrameSharedHeader)) {
        error = L"the display frame header is not a valid DeskFeed frame (magic/version/header size)";
        return false;
    }
    // 容量字段也必须对得上：它们决定读端能安全解释多少字节，只看 magic 不足以确认这块映射的
    // 形状就是我们认识的那一种。
    if (header.capacity != deskfeed::kFrameSharedBytes ||
        header.payload_capacity != deskfeed::kFramePayloadCapacity) {
        error = L"the display frame header reports a different capacity; refusing to use it";
        return false;
    }
    // 未知标志位：只有本文件认识的那些才允许出现。带未知位说明这不是我们认识的那份帧头。
    if ((header.flags & ~deskfeed::kFrameFlagClosed) != 0) {
        error = L"the display frame header carries unknown flags";
        return false;
    }

    out.present = true;
    out.pid = static_cast<unsigned long>(header.producer_pid);
    out.start_time = header.producer_start_time;
    out.generation = static_cast<std::uint32_t>(header.generation);
    out.closed = (header.flags & deskfeed::kFrameFlagClosed) != 0;
    return true;
}

bool query_payload_status(unsigned long process_id, std::wstring const& dll_path,
                          std::wstring const& scope, PayloadStatus& out, std::wstring& error) {
    out = PayloadStatus{};
    error.clear();

    ModuleEntry module;
    std::wstring lookup_error;
    switch (lookup_module_by_full_path(process_id, dll_path, module, lookup_error)) {
        case ModuleLookup::Found:
            break;
        case ModuleLookup::NotFound:
            out.state = PayloadState::NotMapped;
            out.detail = lookup_error;
            return true;
        case ModuleLookup::Error:
            // **不可检查不是"没装载"**：报错让调用方按"问不出来"处理（--status 退出 2）。
            error = L"cannot read the target module list: " + lookup_error;
            return false;
    }
    out.mapped = true;
    out.module = module;

    // 邮箱这一半与 ABI 无关，先读：它是"负载绑定的是不是当前这个生产者"的独立证据。
    MailboxProducer producer;
    std::wstring mailbox_error;
    if (probe_mailbox_producer(scope, producer, mailbox_error)) {
        out.mailbox_present = producer.present;
        out.mailbox_producer_pid = producer.pid;
        out.mailbox_producer_start = producer.start_time;
        out.mailbox_producer_closed = producer.closed;
        if (producer.present) {
            out.mailbox_producer_alive = process_matches_identity(producer.pid, producer.start_time);
        }
    } else {
        out.detail = mailbox_error;
    }

    PeExportError export_kind = PeExportError::None;
    PeExportInfo export_info;
    std::wstring export_error;
    if (!pe_export_rva(dll_path, "DeskFeedGetStatus", export_info, export_error, &export_kind)) {
        out.state = PayloadState::MappedUnknownAbi;
        out.detail = L"the payload reports no lifecycle state: " + export_error;
        return true;
    }
    if (!pe_image_matches_module(export_info, module.size, export_error)) {
        out.state = PayloadState::MappedUnverifiable;
        out.detail = L"the on-disk payload does not match the loaded module: " + export_error;
        return true;
    }
    // 尺寸相同不等于同一个 build：受限读取远程镜像自己的 PE 元数据与导出表，只有同名导出的
    // 远程真实 RVA 与磁盘一致、且落在可执行节内才允许调用；否则就是"读不出可信状态"，绝不因此
    // 报成 NotMapped / active，也绝不允许任何 Stop/eject 走这条路。
    unsigned long verified_rva = 0;
    if (!verify_remote_export(process_id, module.base, module.size, "DeskFeedGetStatus", export_info,
                              verified_rva, export_error)) {
        out.state = PayloadState::MappedUnverifiable;
        out.detail = L"the loaded payload does not match the on-disk DLL: " + export_error;
        return true;
    }

    PayloadStatusPod pod;
    pod.size = sizeof(PayloadStatusPod);
    pod.abi = kPayloadStatusAbi;
    pod.width = kPayloadStatusWidth;  // 显式宽度：让负载侧 ABI 校验也能看出我们在说哪一份布局

    RemoteCall call;
    std::wstring fetch_error;
    if (!remote_fetch_pod(process_id, module.base, verified_rva, &pod, sizeof(pod), 5000, call,
                          fetch_error)) {
        // 超时/未创建/读回失败：结论未知。绝不能当成 Inactive（那会让调用方去 Stop 一个可能正有
        // 远程线程在它代码里执行的模块）。
        out.state = PayloadState::MappedUnverifiable;
        out.detail = L"DeskFeedGetStatus did not answer: " + fetch_error;
        return true;
    }
    if (pod.size != sizeof(PayloadStatusPod) || pod.abi != kPayloadStatusAbi) {
        out.state = PayloadState::MappedUnknownAbi;
        out.detail = L"the payload does not speak status ABI 1 (abi=" +
                     deskfeed::hex_text(pod.abi) + L", size=" + deskfeed::hex_text(pod.size) + L")";
        return true;
    }
    std::wstring pod_scope;
    std::wstring pod_problem;
    if (!pod_is_well_formed(pod, pod_scope, pod_problem)) {
        out.state = PayloadState::MappedUnverifiable;
        out.detail = L"the payload wrote a malformed status POD: " + pod_problem;
        return true;
    }

    out.have_pod = true;
    out.pod = pod;
    out.scope_matches = pod_scope == scope;
    // 走到这里才算"读到了一张可信的 POD"：后面的 Inactive/Active/Retained 都是确定的结论。
    out.conclusive = true;

    // POD 报的生产者身份必须与帧头一致：否则它挂的是**另一个** scope / 另一份邮箱的主人。
    if (out.mailbox_present) {
        out.producer_consistent = pod.producer_pid_low == static_cast<std::uint32_t>(producer.pid) &&
                                  pod.producer_pid_high == 0 &&
                                  pod.producer_start_time == producer.start_time;
    }

    bool const thread_alive = pod.thread_alive != 0;
    bool const retained = pod.dangling != 0 || (pod.started != 0 && !thread_alive && pod.unload_safe == 0);
    if (retained) {
        out.state = PayloadState::MappedRetained;
        if (out.detail.empty()) {
            out.detail = pod.dangling != 0
                             ? L"the payload reports an unproven dangling hook"
                             : L"the payload thread ended without reporting that it is safe to unload";
        }
        return true;
    }

    if (pod.started == 0 || !thread_alive) {
        out.state = PayloadState::MappedInactive;
        if (out.detail.empty()) {
            out.detail = pod.started == 0 ? L"the payload has not been started"
                                          : L"the payload thread is gone";
        }
        return true;
    }

    if (pod.mailbox_attached == 0) {
        out.state = PayloadState::MappedInactive;
        out.detail = L"the payload is not attached to a display mailbox";
        return true;
    }
    if (!out.scope_matches) {
        out.state = PayloadState::MappedInactive;
        out.detail = L"the payload is running on a different scope";
        return true;
    }
    if (!out.mailbox_present) {
        out.state = PayloadState::MappedInactive;
        out.detail = L"the display mailbox for this scope does not exist any more";
        return true;
    }
    if (out.mailbox_producer_closed) {
        // 生产者已经正常收尾：负载此时应当把块摘掉，绝不算"活跃"——哪怕它的线程还赖着没退。
        out.state = PayloadState::MappedInactive;
        out.detail = L"the mailbox producer has closed its last frame";
        return true;
    }
    if (!out.mailbox_producer_alive) {
        out.state = PayloadState::MappedInactive;
        out.detail = L"the bound producer process is gone";
        return true;
    }
    if (!out.producer_consistent) {
        out.state = PayloadState::MappedInactive;
        out.detail = L"the payload's producer identity does not match the live mailbox frame header";
        return true;
    }

    out.state = PayloadState::MappedActive;
    out.detail = L"the payload is running on this scope and bound to the live producer";
    return true;
}

}  // namespace deskfeed::inject
