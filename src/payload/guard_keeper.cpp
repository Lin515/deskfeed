#include "payload/guard_keeper.h"

#include "payload/guard_record.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <iterator>

// 进程守卫对象的落点。**本文件是负载里唯一持有这三个内核对象引用的地方**，也是 Native 侧"不可证就
// 保留映射"这条结论的唯一来源。设计契约见 `payload/guard_record.h` 顶部注释与
// `docs/native-safety.md`；本文件只实现三个动作（prepare / stop_allowed / release），不新增 getter。
//
// 为什么不是"每次 Stop 再 open 一次"：`DllMain` 是唯一"模块在自己进程里刚被发布"的时刻，在那里
// 拿到引用，才能保证之后每一次 `DeskFeedStop`（可能来自另一条远程线程）看到的都是同一份事实；
// 也让 DETACH 有明确的引用归还点。

namespace deskfeed::payload {
namespace {

// 本模块持有的 3 个内核引用 + 1 个**只读**视图。空表示"没拿到/已归还"。
HANDLE g_guard_mutex = nullptr;
HANDLE g_guard_event = nullptr;
HANDLE g_guard_mapping = nullptr;
GuardRecord* g_guard_view = nullptr;

// 粘性"未知"。默认 true：`payload_guard_prepare()` 没成功走完之前，任何 Stop 都不是可证干净的。
bool g_guard_unknown = true;
bool g_guard_prepared = false;

// 只关本模块自己那 3 个引用 + 视图。DETACH 与 prepare 的失败路径共用。
void guard_close_references() noexcept {
    if (g_guard_view != nullptr) {
        ::UnmapViewOfFile(g_guard_view);
        g_guard_view = nullptr;
    }
    if (g_guard_mapping != nullptr) {
        ::CloseHandle(g_guard_mapping);
        g_guard_mapping = nullptr;
    }
    if (g_guard_event != nullptr) {
        ::CloseHandle(g_guard_event);
        g_guard_event = nullptr;
    }
    if (g_guard_mutex != nullptr) {
        ::CloseHandle(g_guard_mutex);
        g_guard_mutex = nullptr;
    }
}

// 坏视图（地址被撤掉/被别的映射覆盖）必须按"不可证"处理，绝不允许访问违例穿回 kernel32：
// `DeskFeedStop` 是远程线程调进来的，穿出去就是 explorer 终止。这是叶子，只读一个 bool。
bool guard_view_header_ok_seh(GuardRecord const* view, bool& header_ok) {
    __try {
        header_ok = guard_header_ok(*view);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// 当前进程的创建时间（FILETIME 的完整 64 位值）。对象名与 actor 身份校验都用它：PID 会被复用，
// 只有 pid+creation 才能标识"还是当初那个进程"。
bool current_process_creation(std::uint64_t& creation) noexcept {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    creation = 0;
    if (::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user) == FALSE) {
        return false;
    }
    ULARGE_INTEGER stamp{};
    stamp.LowPart = created.dwLowDateTime;
    stamp.HighPart = created.dwHighDateTime;
    creation = stamp.QuadPart;
    return creation != 0;
}

}  // namespace

// `DllMain(DLL_PROCESS_ATTACH)` 调用。**加载器锁内**：只允许 kernel32 + 栈上定长缓冲。
//
// 次序与"Native 是不是写者"：
//   1. 名字用唯一的 `guard_format_object_name` 在栈上拼（`wchar_t[128]`，零分配、零截断）；
//   2. `CreateMutexW(FALSE)` 不 own、不 Wait、不 Release；`CreateEventW(TRUE,FALSE)` 手动复位、
//      初始未置位，Native 永不置也永不清；
//   3. `CreateFileMappingW(PAGE_READWRITE, 4096)`：
//      * `!ERROR_ALREADY_EXISTS`（本进程创建）⇒ 一次性 `guard_init_new_record(seq=1)`，随后
//        `UnmapViewOfFile` + 重新 `MapViewOfFile(FILE_MAP_READ)` ⇒ 之后**结构上**不可能再写记录；
//      * 已存在 ⇒ 直接只读视图。
// **"内核创建是原子的"不能推出"actor 必然看不到零 header"**：只有"actor 先 init 再 Load"这条协议
// 规约让次序可证。所以任何失败、错类型对象、或视图上不可变头不认，一律粘性未知 + fail closed，
// **绝不**拿它去复置/覆盖旧的未知状态。
void payload_guard_prepare() noexcept {
    if (g_guard_prepared) {
        return;
    }
    g_guard_prepared = true;

    std::uint64_t creation = 0;
    if (!current_process_creation(creation)) {
        g_guard_unknown = true;  // 拼不出对象名 ⇒ 不可证
        return;
    }

    wchar_t mutex_name[kGuardNameCapacity]{};
    wchar_t event_name[kGuardNameCapacity]{};
    wchar_t record_name[kGuardNameCapacity]{};
    if (!guard_format_object_name(mutex_name, std::size(mutex_name), kGuardSuffixMutex,
                                  static_cast<std::uint32_t>(::GetCurrentProcessId()), creation) ||
        !guard_format_object_name(event_name, std::size(event_name), kGuardSuffixEvent,
                                  static_cast<std::uint32_t>(::GetCurrentProcessId()), creation) ||
        !guard_format_object_name(record_name, std::size(record_name), kGuardSuffixRecord,
                                  static_cast<std::uint32_t>(::GetCurrentProcessId()), creation)) {
        g_guard_unknown = true;
        return;
    }

    g_guard_mutex = ::CreateMutexW(nullptr, FALSE, mutex_name);
    g_guard_event = ::CreateEventW(nullptr, TRUE, FALSE, event_name);
    if (g_guard_mutex == nullptr || g_guard_event == nullptr) {
        guard_close_references();
        g_guard_unknown = true;
        return;
    }

    // 只有"本进程创建"才允许初始化。显式清零 last-error 之后再看 ERROR_ALREADY_EXISTS：这是
    // 判定"我是不是创建者"的唯一机械依据（别把 GetLastError 的残留值当证据）。
    ::SetLastError(ERROR_SUCCESS);
    g_guard_mapping =
        ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, record_name);
    DWORD const mapping_error = ::GetLastError();
    if (g_guard_mapping == nullptr) {
        guard_close_references();
        g_guard_unknown = true;
        return;
    }
    bool const created_here = mapping_error != ERROR_ALREADY_EXISTS;

    if (created_here) {
        void* const writable =
            ::MapViewOfFile(g_guard_mapping, FILE_MAP_WRITE, 0, 0, sizeof(GuardRecord));
        if (writable == nullptr) {
            guard_close_references();
            g_guard_unknown = true;
            return;
        }
        guard_init_new_record(*static_cast<GuardRecord*>(writable), 1);
        ::UnmapViewOfFile(writable);
    }

    // 之后一律只读视图：写它就是访问违例 ⇒ Native 结构上不再是记录的写者。
    void* const readable =
        ::MapViewOfFile(g_guard_mapping, FILE_MAP_READ, 0, 0, sizeof(GuardRecord));
    if (readable == nullptr) {
        guard_close_references();
        g_guard_unknown = true;
        return;
    }
    g_guard_view = static_cast<GuardRecord*>(readable);

    bool header_ok = false;
    if (!guard_view_header_ok_seh(g_guard_view, header_ok) || !header_ok) {
        // 零/坏 header（例如"用别的工具先建了同名对象"或协议被破坏）：永不覆盖、永不 reset。
        // 只放弃自己那份引用，让这个对象在最后一个持有者关掉后自然消失 ⇒ 下一位正常 init 的 actor
        // 才能自愈；本模块这一代永久未知。
        guard_close_references();
        g_guard_unknown = true;
        return;
    }

    g_guard_unknown = false;
}

// `DeskFeedStop` 的第一道门。三个拒绝理由，全部是"不可证"：
//   * 粘性未知（prepare 失败/错类型/坏 header）；
//   * 粘性不确定事件已置位（actor 在不可证的事实下置的；**Native 永不替它清**）；
//   * 不可变头不认（size/version）。
// 事件等待的结论必须显式区分 `WAIT_TIMEOUT`："不是 WAIT_OBJECT_0" 不等于"干净"——`WAIT_FAILED`
// 是"问不出来"，同样是不可证。
//
// **不读 [8,40) 快照**：actor 发起 Stop 之前按写序已经写下 `Pending*`（就是正在执行的这一次），
// 拿它当门会把必须允许的那次调用拒掉。头是一次写入之后只读的，事件是内核对象，两者天然同步。
bool payload_guard_stop_allowed() noexcept {
    if (g_guard_unknown) {
        return false;
    }
    if (g_guard_event == nullptr || g_guard_view == nullptr) {
        return false;
    }
    DWORD const wait = ::WaitForSingleObject(g_guard_event, 0);
    if (wait != WAIT_TIMEOUT) {
        return false;  // WAIT_OBJECT_0 = 粘性不确定；WAIT_FAILED = 不可证
    }
    bool header_ok = false;
    if (!guard_view_header_ok_seh(g_guard_view, header_ok)) {
        return false;  // 坏视图
    }
    return header_ok;
}

// `DllMain(DLL_PROCESS_DETACH)` 调用。只归还本模块自己的引用，**不等待**、不分配、不做任何
// 可能需要加载器/内核之外资源的事。
//
// 为什么"只关自己的"就是对的（两条不变量）：
//   (i) 未知 ⇒ 永不返回 1：唯一合法卸载路径是 `DeskFeedStop` 返回 1 后的 `FreeLibrary`，所以
//       "必须跨 DETACH 存活的事实"与 DETACH 互斥；
//   (ii) clean 的必要条件包含"**上一位** actor 留下的不可证 pending 已经被下一位 actor 在
//       WAIT_ABANDONED 里收敛过"：`PendingNoTid` 与"查不出来的 `PendingTid`"这类**跨事务**的旧
//       pending 一律 retained，而 Native 自己既不置也不清事件。注意这条**不是**"Native 要求记录
//       回到 Idle"：Native 从不读 [8,40) 快照，所以 actor 自己正在执行的那次 Stop 写下的
//       `Pending*` 不影响本次结论（见 `payload_guard_stop_allowed`）。
// healthy actor 自己持着 3 个命名对象句柄 + 视图跨过 `FreeLibrary`，所以 record 不会因为本模块归还
// 引用而消失——这不是"引用永远泄漏"，actor 在 `conclude_clean` 后成对关掉。
void payload_guard_release() noexcept {
    guard_close_references();
    g_guard_unknown = true;
}

}  // namespace deskfeed::payload
