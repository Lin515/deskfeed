#pragma once

// payload guard 内核对象的**唯一**共同定义（E 契约冻结稿 v1）。
//
// 用途：把"某个进程用 CreateRemoteThread 打进来的、可能还在本模块里执行的入口线程"这件事，变成
// 一个**能跨 actor 进程崩溃存活**的命名内核对象事实，好让 `DeskFeedStop` 在不可证时 fail closed
// （返回 0 = 保留映射），而不需要 NtQueryThread class 9、新 daemon、持久文件或 PIN。
//
// **本文件是唯一出处**：负载侧（src/payload）与工具侧（src/inject 等）都 include 它。
// 工具侧**绝不允许**再自己声明一次布局/常量（`src/inject/status_pod.h` 那种两份副本的教训不重演）；
// 需要短名字就用 `using` 别名。本文件只依赖 <cstddef>/<cstdint>，不拉 windows.h、不分配、不抛。
//
// ---------------------------------------------------------------------------
// 对象名（两侧都用 guard_format_object_name 生成，禁止手拼）
//   Local\DeskFeed-payload-guard-<pid>-<creationhex>-mutex
//   Local\DeskFeed-payload-guard-<pid>-<creationhex>-event
//   Local\DeskFeed-payload-guard-<pid>-<creationhex>-record
//   <pid>          = 目标进程 pid，十进制无前导零
//   <creationhex>  = 目标进程创建时间 FILETIME 的完整 64 位值，恰好 16 位**小写**十六进制
//   Local\ = 当前 Session；默认 ACL（创建者令牌 ⇒ 同用户/同 IL）；不查 SID、不建显式 SD
// ---------------------------------------------------------------------------
// 记录布局（40 字节、无指针、版本化）与"两块区域"分工
//
//   偏移  字段        字节  区域              谁写              谁校验
//    0    size         4    不可变头 header   创建者，仅一次    Native: == 40
//    4    version      4    不可变头 header   创建者，仅一次    Native: == 1
//    8    state        4    actor 快照        actor（持租约）   actor: guard_snapshot_ok
//   12    reserved     4    actor 快照        actor（恒 0）     actor: == 0
//   16    tid          8    actor 快照        actor             actor
//   24    seq          8    actor 快照        actor             actor（仅诊断）
//   32    checksum     4    actor 快照        actor             actor: FNV-1a
//   36    reserved2    4    actor 快照        actor（恒 0）     actor: == 0
//
//   * guard_header_ok() 只看 [0,8)：size/version。**Native 只校验这一段 + 粘性事件**，
//     **永不读 actor 快照、尤其永不读 checksum** —— 否则 actor 自己发起 `--stop` 时（按写序它已经
//     记下 Pending*，那就是正在执行的这次 Stop）会被自写快照误判成"有入口在飞"而拒绝，
//     而那正是必须允许走到结论的那次调用。
//   * guard_snapshot_fnv() / guard_snapshot_ok() 只看 [8,40)：整体 FNV-1a（checksum 字段按 0 代入）
//     + state 合法 + reserved/reserved2 == 0 + state 与 tid 自洽。由 actor **持租约**时校验。
// ---------------------------------------------------------------------------
// 写序（actor 是记录的**唯一**写者；违反它整套结论就不成立）
//   PendingNoTid(seq) 必须在 CreateRemoteThread **之前**写入；
//   拿到线程句柄/TID 后写 PendingTid{tid}(seq)；
//   远程调用**真实返回**且线程句柄**真实 signaled**之后才写 Idle(seq)。
//   actor 整段动作持 lease mutex；只在持锁时读/写记录（禁止 lock-free 读：跨进程同一物理页，
//   没有 mutex 就没有 happens-before）。异常/提前 return 一律**不释放** lease（线程退出 ⇒
//   abandoned ⇒ 事实保留）。
// ---------------------------------------------------------------------------
// 初始化（新 mapping 全零 = 未知 = fail closed）
//   * 首选（协议规约）：**actor 先 create + guard_init_new_record() 再 LoadLibraryW**。
//   * Native 只有在"该对象确实由本进程创建"（CreateFileMappingW 未返回 ERROR_ALREADY_EXISTS）时
//     才允许做**一次性** guard_init_new_record()，之后立刻改成只读视图 ⇒ 结构上不再是写者。
//   * **不得**把"内核创建是原子的"当成"actor 必然看不到零 header"：只有"actor 先 init"这条**协议
//     规约**能让次序可证。因此任何"先看到零/坏 header"的路径一律按未知 fail closed（Stop 恒 0），
//     绝不允许把它当成"没有不确定性"，也绝不允许拿它去复置/覆盖旧的未知状态。
//   * direct unit / 探针宿主必须自己先 Setup（本文件的三个 Create* + guard_init_new_record），
//     或走 Native 创建路径；否则就是未知。
// ---------------------------------------------------------------------------
// 恢复（actor 拿到 WAIT_ABANDONED 后，**持锁**读到记录时）
//   Idle                                  -> 清事件 + 重写 Idle，继续
//   PendingTid{tid} + Toolhelp 确认 TID 不存在 -> 清事件 + 重写 Idle，继续
//   PendingTid{tid} + TID 仍在 / 查询失败     -> 保持事件（TID 复用只会 false-retain）
//   PendingNoTid / Unknown                    -> 保持事件（PendingNoTid 本会话永久）
//   Toolhelp（CreateToolhelp32Snapshot + Thread32First/Next，限目标 pid）只在 **actor** 侧用；
//   Native 侧不新增该导入。
// ---------------------------------------------------------------------------
// 范围限定：只对**本协议版本的 cooperating 参与者**成立（本仓库 deskfeed-inject.exe 含主进程派生
// 与 payload DLL）。不成立于：旧版/第三方工具、调试器、手工 FreeLibrary、任何不拿租约的调用者；
// 外部线程入口（DllGetClassObject / XAML tap 回调 / 探针入口）也不在这套门里。不宣称"整进程任意
// 线程都安全"。lease 按 target pid+creation 命名 ⇒ 跨 explorer 不互斥；Local\ ⇒ 跨会话天然隔离。

#include <cstddef>
#include <cstdint>

namespace deskfeed::payload {

// ---- 常量 -----------------------------------------------------------------

inline constexpr std::uint32_t kGuardRecordSize = 40;  // 字节；ABI，只在 version 变化时改
inline constexpr std::uint32_t kGuardRecordVersion = 1;

inline constexpr std::uint32_t kGuardStateIdle = 0;
inline constexpr std::uint32_t kGuardStatePendingNoTid = 1;
inline constexpr std::uint32_t kGuardStatePendingTid = 2;

// Win32 线程 id 是 DWORD。记录里的 `tid` 是 u64（布局对齐需要），但**只允许**放得进 32 位的值：
// 一个超 32 位的值在 actor 侧被截断后会变成一个"看起来不存在"的 TID，从而把 Unknown 错误地清成
// Idle —— 所以超限一律判 Unknown，绝不当作可回收。
inline constexpr std::uint64_t kGuardMaxTid = 0xFFFFFFFFull;

// 两块区域（字节区间，相对记录起点）。
inline constexpr std::size_t kGuardHeaderOffset = 0;
inline constexpr std::size_t kGuardHeaderBytes = 8;
inline constexpr std::size_t kGuardSnapshotOffset = 8;
inline constexpr std::size_t kGuardSnapshotBytes = 32;

// 字段偏移（相对记录起点）——布局冻结，改这里必须同时改 version 与两侧审计。
inline constexpr std::size_t kGuardSizeOffset = 0;
inline constexpr std::size_t kGuardVersionOffset = 4;
inline constexpr std::size_t kGuardStateOffset = 8;
inline constexpr std::size_t kGuardReservedOffset = 12;
inline constexpr std::size_t kGuardTidOffset = 16;
inline constexpr std::size_t kGuardSeqOffset = 24;
inline constexpr std::size_t kGuardChecksumOffset = 32;
inline constexpr std::size_t kGuardReserved2Offset = 36;

// FNV-1a 32（只覆盖 actor 快照那 32 字节，checksum 字段本身按 0 代入）。
inline constexpr std::uint32_t kGuardFnvOffsetBasis = 2166136261u;
inline constexpr std::uint32_t kGuardFnvPrime = 16777619u;

// 名字：实际最长 ≈ 29 + 10 + 1 + 16 + 1 + 6 = 63；给足冗余并禁止截断。
inline constexpr std::size_t kGuardNameCapacity = 128;
inline constexpr wchar_t kGuardObjectPrefix[] = L"Local\\DeskFeed-payload-guard-";
inline constexpr wchar_t kGuardSuffixMutex[] = L"mutex";
inline constexpr wchar_t kGuardSuffixEvent[] = L"event";
inline constexpr wchar_t kGuardSuffixRecord[] = L"record";

// ---- 记录 -----------------------------------------------------------------

#pragma pack(push, 8)
struct GuardRecord {
    std::uint32_t size;       // = 40（不可变头）
    std::uint32_t version;    // = 1 （不可变头）
    std::uint32_t state;      // kGuardState*
    std::uint32_t reserved;   // 恒 0
    std::uint64_t tid;        // PendingTid 时的入口线程 TID（目标进程内）
    std::uint64_t seq;        // actor 自增序号（仅诊断/防撕裂，不参与门禁）
    std::uint32_t checksum;   // FNV-1a(kGuardSnapshotBytes)，自身按 0 代入
    std::uint32_t reserved2;  // 恒 0
};
#pragma pack(pop)

static_assert(sizeof(GuardRecord) == kGuardRecordSize, "GuardRecord 必须是 40 字节的固定 POD");
static_assert(offsetof(GuardRecord, size) == kGuardSizeOffset, "GuardRecord.size 偏移冻结");
static_assert(offsetof(GuardRecord, version) == kGuardVersionOffset, "GuardRecord.version 偏移冻结");
static_assert(offsetof(GuardRecord, state) == kGuardStateOffset, "GuardRecord.state 偏移冻结");
static_assert(offsetof(GuardRecord, reserved) == kGuardReservedOffset, "GuardRecord.reserved 偏移冻结");
static_assert(offsetof(GuardRecord, tid) == kGuardTidOffset, "GuardRecord.tid 偏移冻结");
static_assert(offsetof(GuardRecord, seq) == kGuardSeqOffset, "GuardRecord.seq 偏移冻结");
static_assert(offsetof(GuardRecord, checksum) == kGuardChecksumOffset, "GuardRecord.checksum 偏移冻结");
static_assert(offsetof(GuardRecord, reserved2) == kGuardReserved2Offset, "GuardRecord.reserved2 偏移冻结");

// ---- 纯函数（两侧共用同一实现；用例直接咬这些） -----------------------------

// 不可变头：只有 size/version。**Native 的唯一记录输入**。
inline bool guard_header_ok(GuardRecord const& record) noexcept {
    return record.size == kGuardRecordSize && record.version == kGuardRecordVersion;
}

// actor 快照的整体 FNV-1a 32（checksum 字段按 0 代入）。
inline std::uint32_t guard_snapshot_fnv(GuardRecord const& record) noexcept {
    GuardRecord copy = record;
    copy.checksum = 0;
    auto const* const bytes = reinterpret_cast<unsigned char const*>(&copy) + kGuardSnapshotOffset;
    std::uint32_t hash = kGuardFnvOffsetBasis;
    for (std::size_t index = 0; index < kGuardSnapshotBytes; ++index) {
        hash ^= static_cast<std::uint32_t>(bytes[index]);
        hash *= kGuardFnvPrime;
    }
    return hash;
}

// actor 快照是否可信：头 + 整体 FNV + state 合法 + reserved 归零 + state 与 tid 自洽。
inline bool guard_snapshot_ok(GuardRecord const& record) noexcept {
    if (!guard_header_ok(record)) {
        return false;
    }
    if (record.reserved != 0 || record.reserved2 != 0) {
        return false;
    }
    if (record.state > kGuardStatePendingTid) {
        return false;
    }
    if (record.state == kGuardStatePendingTid) {
        if (record.tid == 0 || record.tid > kGuardMaxTid) {
            return false;  // PendingTid 必须带一个真实的 DWORD TID
        }
    } else if (record.tid != 0) {
        return false;  // Idle / PendingNoTid 不许带 TID
    }
    return record.checksum == guard_snapshot_fnv(record);
}

// 创建者**一次性**初始化：不可变头 + Idle 快照。之后只会再写快照，绝不再写头。
inline void guard_init_new_record(GuardRecord& record, std::uint64_t seq) noexcept {
    record = GuardRecord{};
    record.size = kGuardRecordSize;
    record.version = kGuardRecordVersion;
    record.state = kGuardStateIdle;
    record.seq = seq;
    record.checksum = guard_snapshot_fnv(record);
}

// 以下三个只写 actor 快照（保持不可变头原样）。
inline void guard_make_idle(GuardRecord& record, std::uint64_t seq) noexcept {
    record.state = kGuardStateIdle;
    record.reserved = 0;
    record.tid = 0;
    record.seq = seq;
    record.reserved2 = 0;
    record.checksum = guard_snapshot_fnv(record);
}

// `CreateRemoteThread` **之前**必须已经写入。
inline void guard_make_pending_no_tid(GuardRecord& record, std::uint64_t seq) noexcept {
    record.state = kGuardStatePendingNoTid;
    record.reserved = 0;
    record.tid = 0;
    record.seq = seq;
    record.reserved2 = 0;
    record.checksum = guard_snapshot_fnv(record);
}

// `CreateRemoteThread` 返回并拿到 TID 之后写入。
inline void guard_make_pending_tid(GuardRecord& record, std::uint64_t tid, std::uint64_t seq) noexcept {
    record.state = kGuardStatePendingTid;
    record.reserved = 0;
    record.tid = tid;
    record.seq = seq;
    record.reserved2 = 0;
    record.checksum = guard_snapshot_fnv(record);
}

// 记录的**形状**（只看记录本身，不含 TID 存活事实）。
enum class GuardSnapshot : std::uint32_t {
    Unknown = 0,      // header/checksum/state/reserved 任一不认 ⇒ 不可证
    Idle = 1,
    PendingNoTid = 2,
    PendingTid = 3,
};

inline GuardSnapshot guard_classify(GuardRecord const& record) noexcept {
    if (!guard_snapshot_ok(record)) {
        return GuardSnapshot::Unknown;
    }
    switch (record.state) {
        case kGuardStateIdle:
            return GuardSnapshot::Idle;
        case kGuardStatePendingNoTid:
            return GuardSnapshot::PendingNoTid;
        case kGuardStatePendingTid:
            return GuardSnapshot::PendingTid;
        default:
            return GuardSnapshot::Unknown;
    }
}

// TID 存活事实：由 actor 用官方 Toolhelp 在**目标进程**里查得（本文件不依赖 Toolhelp）。
enum class GuardTidProbe : std::uint32_t {
    NotQueried = 0,
    Absent = 1,        // 该 TID 在目标进程里不存在
    Present = 2,       // 仍在（含 TID 复用：方向是 fail closed）
    LookupFailed = 3,  // 快照/枚举失败
};

// actor 拿到 WAIT_ABANDONED 后（**持锁**读到记录）该做什么。
enum class GuardRecovery : std::uint32_t {
    KeepUncertain = 0,     // 置/保持粘性事件；本次不卸载、不做动作、绝不清事件
    ClearAndContinue = 1,  // 能证干净：清事件 + 把记录重写为 Idle(seq+1)，继续本次动作
};

inline GuardRecovery guard_recovery(GuardSnapshot snapshot, GuardTidProbe tid_probe) noexcept {
    if (snapshot == GuardSnapshot::Idle) {
        // 死掉的 actor 还没来得及创建任何入口线程（写序保证）；也覆盖"卸载其实已成功、actor 在
        // conclude_clean 前崩掉"这类良性 abandoned 误报。
        return GuardRecovery::ClearAndContinue;
    }
    if (snapshot == GuardSnapshot::PendingTid && tid_probe == GuardTidProbe::Absent) {
        // 那个入口线程已经彻底不在目标进程里了。
        return GuardRecovery::ClearAndContinue;
    }
    return GuardRecovery::KeepUncertain;
}

// ---- 对象名（零分配；两侧共用，禁止手拼） -----------------------------------

inline std::size_t guard_name_length(wchar_t const* text) noexcept {
    std::size_t length = 0;
    // 先判界再解引用：`text` 恰好 kGuardNameCapacity 个字符且没有 NUL 时，绝不能读到索引
    // kGuardNameCapacity（那已经是缓冲之外）。
    while (text != nullptr && length < kGuardNameCapacity && text[length] != L'\0') {
        ++length;
    }
    return length;
}

// 成功：写入 NUL 结尾串并返回 true。容量不足：返回 false，**绝不**截断成另一个名字。
inline bool guard_format_object_name(wchar_t* buffer, std::size_t capacity, wchar_t const* suffix,
                                     std::uint32_t pid, std::uint64_t creation_filetime) noexcept {
    if (buffer == nullptr || suffix == nullptr || capacity == 0) {
        return false;
    }
    std::size_t const prefix_length = guard_name_length(kGuardObjectPrefix);
    std::size_t const suffix_length = guard_name_length(suffix);

    // pid：十进制，无前导零。
    wchar_t digits[10]{};
    std::size_t digit_count = 0;
    std::uint32_t remaining = pid;
    do {
        digits[digit_count++] = static_cast<wchar_t>(L'0' + (remaining % 10u));
        remaining /= 10u;
    } while (remaining != 0 && digit_count < sizeof(digits) / sizeof(digits[0]));

    // creation FILETIME：完整 64 位，16 位小写十六进制。
    wchar_t hex[16]{};
    for (std::size_t index = 0; index < 16; ++index) {
        std::uint32_t const shift = static_cast<std::uint32_t>((15u - index) * 4u);
        std::uint32_t const nibble = static_cast<std::uint32_t>((creation_filetime >> shift) & 0xFull);
        hex[index] = static_cast<wchar_t>(nibble < 10u ? (L'0' + nibble) : (L'a' + (nibble - 10u)));
    }

    std::size_t const total = prefix_length + digit_count + 1 + 16 + 1 + suffix_length;
    if (total + 1 > capacity) {
        return false;
    }

    std::size_t out = 0;
    for (std::size_t index = 0; index < prefix_length; ++index) {
        buffer[out++] = kGuardObjectPrefix[index];
    }
    for (std::size_t index = 0; index < digit_count; ++index) {
        buffer[out++] = digits[digit_count - 1 - index];
    }
    buffer[out++] = L'-';
    for (std::size_t index = 0; index < 16; ++index) {
        buffer[out++] = hex[index];
    }
    buffer[out++] = L'-';
    for (std::size_t index = 0; index < suffix_length; ++index) {
        buffer[out++] = suffix[index];
    }
    buffer[out] = L'\0';
    return true;
}

}  // namespace deskfeed::payload
