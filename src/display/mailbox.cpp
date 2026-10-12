#include "display/mailbox.h"

#include "core/utf8.h"
#include "platform/paths.h"
#include "platform/win32_util.h"

#include <cstring>
#include <vector>

// 跨进程 seqlock：`sequence` 为奇数表示“正在写”，写完帧体与帧头之后置回偶数，最后才叫醒
// 读端。读端比较写入前后两次序号，不一致就重试（有界重试，失败时沿用上一次成功的帧）。
// x86 上 InterlockedIncrement 带完整内存屏障，这里再显式加 MemoryBarrier 把意图写清楚。
//
// 载荷区只有**一份**：新帧覆盖旧帧，所以邮箱里永远是“最新完整帧”，不存在积压。

namespace deskfeed {

std::wstring display_frame_map_name(std::wstring const& scope) {
    return scoped_name(L"Local\\DeskFeed-display-frame", scope);
}

std::wstring display_frame_ready_name(std::wstring const& scope) {
    return scoped_name(L"Local\\DeskFeed-display-ready", scope);
}

namespace {

constexpr int kReadAttempts = 16;
// attach 时帧头可能正被生产者初始化（magic 还没写上去）。有界重试几次，不行就报“尚未就绪”。
constexpr int kAttachAttempts = 32;

// kind / failure 的稳定编号只有这一份：写端与读端都走它，读端因此不会长出第二套解释。
std::uint32_t kind_code(ViewKind kind) {
    switch (kind) {
        case ViewKind::Waiting: return 0;
        case ViewKind::Content: return 1;
        case ViewKind::Error: return 2;
    }
    return 0xFFFFFFFFu;
}

std::uint32_t failure_code(FailureKind failure) {
    switch (failure) {
        case FailureKind::None: return 0;
        case FailureKind::NoUrl: return 1;
        case FailureKind::Transport: return 2;
        case FailureKind::Timeout: return 3;
        case FailureKind::HttpStatus: return 4;
        case FailureKind::Redirect: return 5;
        case FailureKind::TooLarge: return 6;
        case FailureKind::Encoding: return 7;
        case FailureKind::Parse: return 8;
        case FailureKind::Validation: return 9;
        case FailureKind::Internal: return 10;
    }
    return 0xFFFFFFFFu;
}

bool kind_code_known(std::uint32_t code) { return code <= 2; }
bool failure_code_known(std::uint32_t code) { return code <= 10; }

// 一次读期望的生产者身份。pid == 0 表示不校验（生产者自己收尾时用）。
struct ExpectedOwner {
    std::uint64_t pid = 0;
    std::uint64_t start_time = 0;
};

std::uint64_t filetime_value(FILETIME const& time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

std::uint64_t own_process_start_time() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user) == FALSE) {
        return 0;
    }
    return filetime_value(creation);
}

// 打开进程并读出它的创建时间。句柄是 SYNCHRONIZE 的，读端可以直接等它退出。
bool open_process_with_start_time(std::uint64_t pid, HANDLE& handle_out, std::uint64_t& time_out,
                                 std::string& error) {
    handle_out = nullptr;
    time_out = 0;
    if (pid == 0 || pid > 0xFFFFFFFFull) {
        error = "帧头里的生产者 PID 不合法";
        return false;
    }
    HANDLE const process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         static_cast<DWORD>(pid));
    if (process == nullptr) {
        error = "打不开生产者进程 " + std::to_string(pid) + "：" +
                utf8_from_wide(last_error_text());
        return false;
    }
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (::GetProcessTimes(process, &creation, &exit, &kernel, &user) == FALSE) {
        error = "读不到生产者进程的创建时间：" + utf8_from_wide(last_error_text());
        ::CloseHandle(process);
        return false;
    }
    handle_out = process;
    time_out = filetime_value(creation);
    return true;
}

std::uint32_t fnv1a32(unsigned char const* bytes, std::size_t size) {
    std::uint32_t hash = 2166136261u;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 16777619u;
    }
    return hash;
}

// 只读的一次读：读端把共享内存映射成**只读**，所以这里绝不能用 Interlocked* 那种
// 读改写（它在只读页上会直接访问违例）。32 位对齐读在 x86 上不会撕裂，后面再加内存屏障
// 保证帧体/帧头不会被重排到这次读之前。
std::uint32_t read_sequence(FrameSharedHeader const* header) {
    auto const* const sequence = reinterpret_cast<volatile std::uint32_t const*>(&header->sequence);
    std::uint32_t const value = *sequence;
    ::MemoryBarrier();
    return value;
}

// 从一块映射读最新一帧。读端与生产者的收尾路径共用这一份。
//
// 每读一次都要**完整地**过一遍判据：不可变帧头字段、全部已知枚举与标志位、重复元数据与帧体
// 一致、序号前后稳定、载荷能解码、校验和对得上。只做其中一部分不行——attach 通过之后，同一个
// section 仍可能被改写（旧生产者没退干净，或同名 section 被新生产者重建起来）。
bool read_frame_view(unsigned char const* base, std::size_t mapped_bytes, Frame& out,
                     ExpectedOwner const& owner) {
    if (base == nullptr || mapped_bytes < kFrameSharedBytes) {
        return false;
    }
    auto const* const header = reinterpret_cast<FrameSharedHeader const*>(base);
    unsigned char const* const payload = base + sizeof(FrameSharedHeader);

    for (int attempt = 0; attempt < kReadAttempts; ++attempt) {
        std::uint32_t const before = read_sequence(header);
        if ((before % 2) != 0) {
            continue;  // 生产者正在写
        }

        // 不可变帧头字段：与 attach 同一套判据，只是每次读都重新来一遍。
        if (header->magic != kFrameMagic || header->version != kFrameVersion ||
            header->header_bytes != sizeof(FrameSharedHeader) ||
            header->capacity != static_cast<std::uint32_t>(mapped_bytes) ||
            header->payload_capacity != static_cast<std::uint32_t>(kFramePayloadCapacity)) {
            continue;
        }

        std::uint32_t const payload_bytes = header->payload_bytes;
        std::uint32_t const item_count = header->item_count;
        std::uint32_t const kind = header->kind;
        std::uint32_t const failure = header->failure;
        std::uint32_t const flags = header->flags;
        std::uint32_t const alarm = header->alarm;
        std::uint32_t const checksum = header->checksum;
        std::uint64_t const generation = header->generation;
        std::uint64_t const produced_at_ms = header->produced_at_ms;
        std::uint64_t const producer_pid = header->producer_pid;
        std::uint64_t const producer_start_time = header->producer_start_time;

        // 已知枚举与标志位：不认识的一律拒绝，不“尽量解释”。
        if (payload_bytes > kFramePayloadCapacity || item_count > kMaxItems || alarm > 1 ||
            (flags & ~kFrameFlagClosed) != 0 || !kind_code_known(kind) ||
            !failure_code_known(failure)) {
            continue;
        }

        // 身份：帧头必须仍然是 attach 时绑定的那一个生产者。
        if (owner.pid != 0 &&
            (producer_pid != owner.pid || producer_start_time != owner.start_time)) {
            continue;
        }

        Frame decoded;
        if (!frame_decode(payload, payload_bytes, decoded)) {
            continue;
        }
        if (fnv1a32(payload, payload_bytes) != checksum) {
            continue;
        }

        // 帧头与帧体里的重复元数据必须一致：两者是同一份 Frame 写出来的，对不上说明这块内存
        // 已经被别的东西改过（或写到一半），不能当成一帧交出去。
        if (static_cast<std::uint32_t>(decoded.items.size()) != item_count ||
            kind_code(decoded.kind) != kind || failure_code(decoded.failure) != failure ||
            (decoded.alarm ? 1u : 0u) != alarm) {
            continue;
        }

        if (read_sequence(header) != before) {
            continue;  // 读完之前生产者又写了一帧：重试
        }

        decoded.generation = generation;
        decoded.produced_at_ms = produced_at_ms;
        decoded.closed = (flags & kFrameFlagClosed) != 0;
        out = std::move(decoded);
        return true;
    }
    return false;
}

}  // namespace

DisplayFrameProducer::~DisplayFrameProducer() { close(); }

bool DisplayFrameProducer::create(std::wstring const& scope, UserSecurity const* security,
                                 std::string& error) {
    close();
    map_name_ = display_frame_map_name(scope);
    ready_name_ = display_frame_ready_name(scope);

    SECURITY_ATTRIBUTES const* attributes = security != nullptr ? security->attributes() : nullptr;

    // 先建事件、再建共享内存：读端只要看到共享内存，事件就一定已经存在。
    ready_ = ::CreateEventW(const_cast<SECURITY_ATTRIBUTES*>(attributes), FALSE, FALSE,
                            ready_name_.c_str());
    if (ready_ == nullptr) {
        error = "建不了显示邮箱事件：" + utf8_from_wide(last_error_text());
        close();
        return false;
    }

    mapping_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, const_cast<SECURITY_ATTRIBUTES*>(attributes),
                                    PAGE_READWRITE, 0, static_cast<DWORD>(kFrameSharedBytes),
                                    map_name_.c_str());
    if (mapping_ == nullptr) {
        error = "建不了显示邮箱共享内存：" + utf8_from_wide(last_error_text());
        close();
        return false;
    }

    void* const view = ::MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                                       kFrameSharedBytes);
    if (view == nullptr) {
        error = "映射不了显示邮箱：" + utf8_from_wide(last_error_text());
        close();
        return false;
    }
    view_ = static_cast<FrameSharedHeader*>(view);
    capacity_ = kFrameSharedBytes;

    std::memset(view_, 0, sizeof(FrameSharedHeader));
    start_time_ = own_process_start_time();
    // 初始化顺序是有意的：**先把身份字段写全，最后才写 magic**。magic 一旦就位，读端就认为
    // 这块内存可用；若身份还没写，读端会看到 pid == 0。旧实现把身份放在最后写，于是留下一个
    // “帧头看似合法、身份却还没写好”的真实竞态窗口（attach 会以难懂的错误失败）。
    view_->producer_pid = ::GetCurrentProcessId();
    view_->producer_start_time = start_time_;
    view_->header_bytes = static_cast<std::uint32_t>(sizeof(FrameSharedHeader));
    view_->capacity = static_cast<std::uint32_t>(capacity_);
    view_->payload_capacity = static_cast<std::uint32_t>(kFramePayloadCapacity);
    view_->sequence = 0;
    generation_ = 0;
    ::MemoryBarrier();
    view_->version = kFrameVersion;
    view_->magic = kFrameMagic;  // 最后一步：magic 就位 = 整份帧头可读
    return true;
}

void DisplayFrameProducer::close() {
    if (view_ != nullptr) {
        ::UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_ != nullptr) {
        ::CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    if (ready_ != nullptr) {
        ::CloseHandle(ready_);
        ready_ = nullptr;
    }
    capacity_ = 0;
    view_ = nullptr;
}

bool DisplayFrameProducer::write_frame(Frame const& frame, std::uint32_t extra_flags,
                                       long long now_ms) {
    if (view_ == nullptr) {
        return false;
    }
    std::vector<unsigned char> payload;
    if (!frame_encode(frame, payload)) {
        return false;
    }
    if (payload.size() > kFramePayloadCapacity) {
        return false;
    }

    auto* const bytes = reinterpret_cast<unsigned char*>(view_);
    FrameSharedHeader* const header = view_;

    ::InterlockedIncrement(reinterpret_cast<volatile LONG*>(&header->sequence));  // 奇：正在写
    // 先写帧体，再写帧头与代次——读端只有在序号回到偶数时才认这一帧。
    std::memcpy(bytes + sizeof(FrameSharedHeader), payload.data(), payload.size());
    header->payload_bytes = static_cast<std::uint32_t>(payload.size());
    header->checksum = fnv1a32(payload.data(), payload.size());
    header->item_count = static_cast<std::uint32_t>(frame.items.size());
    header->kind = kind_code(frame.kind);
    header->alarm = frame.alarm ? 1u : 0u;
    header->failure = failure_code(frame.failure);
    header->flags = extra_flags;
    header->generation = ++generation_;
    header->produced_at_ms = static_cast<std::uint64_t>(now_ms);
    header->producer_pid = ::GetCurrentProcessId();
    header->producer_start_time = start_time_;
    ::MemoryBarrier();
    ::InterlockedIncrement(reinterpret_cast<volatile LONG*>(&header->sequence));  // 偶：稳定
    ::SetEvent(ready_);
    return true;
}

bool DisplayFrameProducer::publish(Frame const& frame, long long now_ms) {
    return write_frame(frame, kFrameFlagNone, now_ms);
}

bool DisplayFrameProducer::publish_closed(long long now_ms) {
    if (view_ == nullptr) {
        return false;
    }
    // 保留最后一帧的内容，只把 closed 标志置上：读端据此清理任务栏组件。
    // 复用与读端同一份校验过的读取逻辑，不再自己拼一份枚举映射（那是第二套解释）。
    Frame last;
    ExpectedOwner const self{static_cast<std::uint64_t>(::GetCurrentProcessId()), start_time_};
    if (!read_frame_view(reinterpret_cast<unsigned char const*>(view_), capacity_, last, self)) {
        last = Frame{};  // 还没发布过任何帧：发一帧空的 waiting + closed
    }
    return write_frame(last, kFrameFlagClosed, now_ms);
}

DisplayFrameConsumer::~DisplayFrameConsumer() { detach(); }

bool DisplayFrameConsumer::attach(std::wstring const& scope, std::string& error) {
    detach();
    std::wstring const map_name = display_frame_map_name(scope);
    std::wstring const ready_name = display_frame_ready_name(scope);

    mapping_ = ::OpenFileMappingW(FILE_MAP_READ, FALSE, map_name.c_str());
    if (mapping_ == nullptr) {
        error = "找不到显示邮箱共享内存：" + utf8_from_wide(last_error_text());
        detach();
        return false;
    }
    void const* const view = ::MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, kFrameSharedBytes);
    if (view == nullptr) {
        error = "映射不了显示邮箱：" + utf8_from_wide(last_error_text());
        detach();
        return false;
    }
    view_ = static_cast<unsigned char const*>(view);
    capacity_ = kFrameSharedBytes;

    // 帧头快照：生产者可能正处在 create() 的初始化中间（身份与 magic 是分开写的），所以这里在
    // 一次稳定读里同时取不可变字段与身份，取不齐就重试。重试仍不行时报“尚未就绪”这种**可重试**
    // 的原因，而不是把半写的帧头当成损坏（旧实现就是在这里以难懂的错误失败）。
    bool header_ready = false;
    bool header_mismatch = false;
    std::uint64_t producer_pid = 0;
    std::uint64_t producer_start_time = 0;
    for (int attempt = 0; attempt < kAttachAttempts && !header_ready && !header_mismatch; ++attempt) {
        auto const* const header = reinterpret_cast<FrameSharedHeader const*>(view_);
        std::uint32_t const before = read_sequence(header);
        if ((before % 2) != 0) {
            continue;  // 生产者正在写
        }
        std::uint32_t const magic = header->magic;
        std::uint32_t const version = header->version;
        std::uint32_t const header_bytes = header->header_bytes;
        std::uint32_t const capacity = header->capacity;
        std::uint32_t const payload_capacity = header->payload_capacity;
        producer_pid = header->producer_pid;
        producer_start_time = header->producer_start_time;
        if (read_sequence(header) != before) {
            continue;
        }
        if (magic == 0) {
            continue;  // 还没初始化完：下一次再试
        }
        if (magic != kFrameMagic || version != kFrameVersion ||
            header_bytes != sizeof(FrameSharedHeader) || capacity != capacity_ ||
            payload_capacity != kFramePayloadCapacity) {
            // magic 已经就位却与当前版本不符：这不是“没就绪”，是另一个版本的东西。
            header_mismatch = true;
            break;
        }
        if (producer_pid == 0 || producer_start_time == 0) {
            continue;  // 帧头合法但身份还没写全：等它写完
        }
        header_ready = true;
    }
    if (header_mismatch) {
        error = "显示邮箱的帧头与当前版本不一致";
        detach();
        return false;
    }
    if (!header_ready) {
        error = "显示邮箱的生产者尚未就绪（帧头还没有完整写出）";
        detach();
        return false;
    }

    // 身份校验：PID 会被复用，所以必须比对进程创建时间，并用这个活动句柄等它退出。
    std::uint64_t start_time = 0;
    if (!open_process_with_start_time(producer_pid, producer_, start_time, error)) {
        detach();
        return false;
    }
    if (start_time == 0 || start_time != producer_start_time) {
        error = "显示邮箱属于另一个（或已经不存在的）进程，PID 已被复用";
        detach();
        return false;
    }
    // 绑定的身份来自**活动句柄**（不是帧头自述），之后每一次读都拿它跟帧头比对。
    owner_pid_ = producer_pid;
    owner_start_time_ = start_time;

    ready_ = ::OpenEventW(SYNCHRONIZE, FALSE, ready_name.c_str());
    if (ready_ == nullptr) {
        error = "打不开显示邮箱事件：" + utf8_from_wide(last_error_text());
        detach();
        return false;
    }
    return true;
}

void DisplayFrameConsumer::detach() {
    if (view_ != nullptr) {
        ::UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_ != nullptr) {
        ::CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    if (ready_ != nullptr) {
        ::CloseHandle(ready_);
        ready_ = nullptr;
    }
    if (producer_ != nullptr) {
        ::CloseHandle(producer_);
        producer_ = nullptr;
    }
    owner_pid_ = 0;
    owner_start_time_ = 0;
    capacity_ = 0;
}

bool DisplayFrameConsumer::producer_running() const {
    if (producer_ == nullptr) {
        return false;
    }
    return ::WaitForSingleObject(producer_, 0) == WAIT_TIMEOUT;
}

bool DisplayFrameConsumer::read(Frame& out) const {
    if (view_ == nullptr) {
        return false;
    }
    // 绑定的生产者已经退出：这块映射可能还活着（还有别的进程持着这个 section），但里面那
    // 最后一帧已经不是活数据了。**绝不能**交出去——那正是“主进程已死、任务栏还在画陈旧读数”。
    // 调用方据此清理任务栏组件；正常收尾（publish_closed）与崩溃走的是同一个动作。
    if (!producer_running()) {
        return false;
    }
    return read_frame_view(view_, capacity_, out, ExpectedOwner{owner_pid_, owner_start_time_});
}

}  // namespace deskfeed
