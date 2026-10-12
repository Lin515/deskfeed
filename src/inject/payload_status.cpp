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

std::wstring scope_of_pod(PayloadStatusPod const& pod) {
    return std::wstring(pod.scope, pod_scope_length(pod));
}

// POD 里的布尔字段只能是 0 或 1：别的值说明这份 POD 不是我们认识的那一份（或已经被写坏）。
bool is_flag(std::uint32_t value) { return value == 0 || value == 1; }

// 校验负载写回来的 POD：`size`/`abi` 之外，还要看每一个字段的取值域与 scope 串本身。
bool pod_is_well_formed(PayloadStatusPod const& pod, std::wstring& why) {
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
    if (pod.scope_length >= kPayloadScopeCapacity) {
        // 规范里 scope 最多 32 个字符；长度到容量上限（= 串在容量内没有结尾）说明这不是规范值，
        // 不能把被截断的 35 字符串当成合法的 32 字符 scope。
        why = L"scope_length is out of range";
        return false;
    }
    bool const terminated = pod.scope[pod.scope_length] == L'\0';
    if (!terminated) {
        why = L"the scope string is not NUL terminated at scope_length";
        return false;
    }
    for (std::uint32_t index = 0; index < pod.scope_length; ++index) {
        wchar_t const character = pod.scope[index];
        bool const allowed = (character >= L'A' && character <= L'Z') ||
                             (character >= L'a' && character <= L'z') ||
                             (character >= L'0' && character <= L'9') || character == L'_' ||
                             character == L'-';
        if (!allowed) {
            why = L"the scope string contains a character outside the allowed set";
            return false;
        }
    }
    return true;
}

}  // namespace

char const* payload_state_name(PayloadState state) {
    switch (state) {
        case PayloadState::NotMapped: return "not-mapped";
        case PayloadState::MappedUnknownAbi: return "mapped-unknown-abi";
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
        out.state = PayloadState::MappedUnknownAbi;
        out.detail = L"the on-disk payload does not match the loaded module: " + export_error;
        return true;
    }

    PayloadStatusPod pod;
    pod.size = sizeof(PayloadStatusPod);
    pod.abi = kPayloadStatusAbi;

    RemoteCall call;
    std::wstring fetch_error;
    if (!remote_fetch_pod(process_id, module.base, export_info.rva, &pod, sizeof(pod), 5000, call,
                          fetch_error)) {
        out.state = PayloadState::MappedUnknownAbi;
        out.detail = L"DeskFeedGetStatus did not answer: " + fetch_error;
        return true;
    }
    if (pod.size != sizeof(PayloadStatusPod) || pod.abi != kPayloadStatusAbi) {
        out.state = PayloadState::MappedUnknownAbi;
        out.detail = L"the payload does not speak status ABI 1 (abi=" +
                     deskfeed::hex_text(pod.abi) + L", size=" + deskfeed::hex_text(pod.size) + L")";
        return true;
    }
    std::wstring pod_problem;
    if (!pod_is_well_formed(pod, pod_problem)) {
        out.state = PayloadState::MappedUnknownAbi;
        out.detail = L"the payload wrote a malformed status POD: " + pod_problem;
        return true;
    }

    out.have_pod = true;
    out.pod = pod;
    out.scope_matches = scope_of_pod(pod) == scope;

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
