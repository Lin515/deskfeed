#include "inject/guard.h"

#include "inject/remote.h"
#include "platform/win32_util.h"

#include <tlhelp32.h>

#include <iterator>

namespace deskfeed::inject {
namespace {

// 测试专用变异开关：**只在被显式定义了这个宏的那一份副本里**生效（本地 ignored 工程里的
// `deskfeed-tools-tests-mutation1/2` 目标）。交付产物不含它；这里留着是为了让"释放谓词真的会咬"
// 这件事有机器证据，而不是只靠读代码。
//   1 = conclude_clean 跳过谓词（提前释放）
//   2 = abandon 路径也释放租约（把未知吞成干净）
//   3 = 正常持有（WAIT_OBJECT_0）时无视粘性事件，把它当"良性残留"清掉并把租约当干净
#ifndef DESKFEED_TOOLS_GUARD_MUTATION
#define DESKFEED_TOOLS_GUARD_MUTATION 0
#endif

namespace payload = deskfeed::payload;

bool event_is_signaled(HANDLE event) {
    return event != nullptr && ::WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

struct NamedObjects {
    std::wstring mutex_name;
    std::wstring event_name;
    std::wstring record_name;
};

// 三个对象名一律经唯一出处的 `guard_format_object_name` 生成，**禁止手拼**（名字拼错会让两侧
// 各自操作不同的内核对象，而且看起来"一切正常"）。
bool format_names(GuardTarget const& target, NamedObjects& out, std::wstring& error) {
    wchar_t buffer[payload::kGuardNameCapacity]{};
    auto const make = [&](wchar_t const* suffix, std::wstring& into) {
        if (!payload::guard_format_object_name(buffer, std::size(buffer), suffix, target.process_id,
                                               target.creation_time)) {
            error = L"cannot format the payload guard object name (capacity too small)";
            return false;
        }
        into.assign(buffer);
        return true;
    };
    return make(payload::kGuardSuffixMutex, out.mutex_name) &&
           make(payload::kGuardSuffixEvent, out.event_name) &&
           make(payload::kGuardSuffixRecord, out.record_name);
}

}  // namespace

GuardTidProbe guard_probe_thread(unsigned long process_id, std::uint64_t tid) {
    // 零、或超出 32 位：不可证。截断之后会变成一个"看起来不存在"的 TID，从而把 Unknown 错误地
    // 清成 Idle，所以这里绝不返回 Absent。
    if (process_id == 0 || tid == 0 || tid > payload::kGuardMaxTid) {
        return GuardTidProbe::LookupFailed;
    }

    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return GuardTidProbe::LookupFailed;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    GuardTidProbe result = GuardTidProbe::LookupFailed;
    if (::Thread32First(snapshot, &entry)) {
        result = GuardTidProbe::Absent;
        do {
            if (entry.th32OwnerProcessID == process_id &&
                static_cast<std::uint64_t>(entry.th32ThreadID) == tid) {
                result = GuardTidProbe::Present;
                break;
            }
        } while (::Thread32Next(snapshot, &entry));
        // 正常走完枚举时最后那次 Thread32Next 失败会置 ERROR_NO_MORE_FILES；**别的**错误码
        // 说明枚举中途断了，此时"没找到"不等于"不存在"——返回 Absent 会把 Unknown 错误地
        // 清成 Idle，所以必须按不可证处理。
        if (result == GuardTidProbe::Absent && ::GetLastError() != ERROR_NO_MORE_FILES) {
            result = GuardTidProbe::LookupFailed;
        }
    }
    ::CloseHandle(snapshot);
    return result;
}

bool guard_target_identity(unsigned long process_id, GuardTarget& out, std::wstring& error) {
    out = GuardTarget{};
    std::uint64_t creation_time = 0;
    if (!process_creation_time(process_id, creation_time, error)) {
        return false;
    }
    if (creation_time == 0) {
        error = L"the target process creation time reads as 0; refusing to use the pid as identity";
        return false;
    }
    out.process_id = process_id;
    out.creation_time = creation_time;
    return true;
}

GuardLease::~GuardLease() {
    if (mutex_ != nullptr && !released_ && !abandoned_) {
        // 默认收尾 = 未知（不是"干净"）：置粘性事件，**绝不**在这里 ReleaseMutex。
        if (event_ != nullptr) {
            ::SetEvent(event_);
        }
        abandoned_ = true;
    }
    if (abandoned_) {
        // 故意一个句柄都不关：abandoned 的 mutex + 粘性事件 + Pending 记录就是"上一位 actor 没
        // 做完"这条事实的载体（生产里负载 DLL 也长期持一份）。短命注入器不需要清理它；在长寿命
        // 进程里这是一次**有意**的句柄泄露，绝不用 CloseHandle 去伪造"干净"。
        return;
    }
    close_all_handles();
}

void GuardLease::close_all_handles() noexcept {
    if (record_ != nullptr) {
        ::UnmapViewOfFile(record_);
        record_ = nullptr;
    }
    if (mapping_ != nullptr) {
        ::CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    if (event_ != nullptr) {
        ::CloseHandle(event_);
        event_ = nullptr;
    }
    if (mutex_ != nullptr) {
        ::CloseHandle(mutex_);
        mutex_ = nullptr;
    }
    owner_thread_ = 0;
}

GuardAcquire GuardLease::acquire(GuardTarget const& target, std::wstring& error) {
    error.clear();
    if (mutex_ != nullptr) {
        // 一个实例只能获取一次。**abandon 过的实例绝不允许复用**：它的三份句柄就是"未知"这条
        // 事实的载体（close_all_handles() 会把它们关掉），而且同一条线程再次 acquire 会按 Win32
        // 语义**递归**拿到同一把 mutex，于是"上一位 actor 崩了"就被它自己抹平了。
        // 复用 abandoned 实例的调用方要的是新的一次动作——那必须构造新的 GuardLease，并且先让
        // 本线程真正结束（abandon 的语义就是"让线程退出"）。
        error = abandoned_ ? L"this lease already ended as unknown; it must not be reused"
                           : L"this lease is already held";
        return GuardAcquire::Error;
    }
    if (target.process_id == 0 || target.creation_time == 0) {
        error = L"the guard target has no identity; refusing to use a bare pid";
        return GuardAcquire::Error;
    }

    // 走到这里 mutex_ 一定是空的（所有失败/收尾路径要么 close_all_handles，要么保留事实句柄），
    // 所以这里不会关掉任何"事实载体"。
    close_all_handles();
    target_ = target;
    released_ = false;
    abandoned_ = false;
    pending_ = false;
    conclusive_ = false;
    seq_ = 0;

    NamedObjects names;
    if (!format_names(target, names, error)) {
        return GuardAcquire::Error;
    }

    mutex_ = ::CreateMutexW(nullptr, FALSE, names.mutex_name.c_str());
    if (mutex_ == nullptr) {
        error = L"CreateMutexW failed: " + deskfeed::last_error_text();
        return GuardAcquire::Error;
    }

    DWORD const wait = ::WaitForSingleObject(mutex_, 0);
    bool was_abandoned = false;
    if (wait == WAIT_OBJECT_0) {
        was_abandoned = false;
    } else if (wait == WAIT_ABANDONED) {
        was_abandoned = true;
    } else if (wait == WAIT_TIMEOUT) {
        // 另一个 actor 正在动作：不排队、不强抢、什么都不碰。
        error = L"another actor holds the payload guard lease for this target";
        close_all_handles();
        return GuardAcquire::Busy;
    } else {
        error = L"WaitForSingleObject(mutex) failed: " + deskfeed::last_error_text();
        close_all_handles();
        return GuardAcquire::Error;
    }
    owner_thread_ = ::GetCurrentThreadId();

    // 从这里开始我们**持有** mutex：之后任何失败路径都只允许"不释放"（abandon），禁止 ReleaseMutex。
    event_ = ::CreateEventW(nullptr, TRUE, FALSE, names.event_name.c_str());
    if (event_ == nullptr) {
        error = L"CreateEventW failed: " + deskfeed::last_error_text();
        abandoned_ = true;
        return GuardAcquire::Error;
    }

    ::SetLastError(0);
    mapping_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    payload::kGuardRecordSize, names.record_name.c_str());
    if (mapping_ == nullptr) {
        error = L"CreateFileMappingW failed: " + deskfeed::last_error_text();
        abandoned_ = true;
        return GuardAcquire::Error;
    }
    bool const created_now = ::GetLastError() != ERROR_ALREADY_EXISTS;

    void* const view =
        ::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, payload::kGuardRecordSize);
    if (view == nullptr) {
        error = L"MapViewOfFile failed: " + deskfeed::last_error_text();
        abandoned_ = true;
        return GuardAcquire::Error;
    }
    record_ = static_cast<GuardRecord*>(view);

    if (created_now) {
        // 协议规约：**actor 先 create + init，然后才 LoadLibraryW**。之后只会再写快照，不再写头。
        payload::guard_init_new_record(*record_, 1);
        seq_ = 1;
    }

    if (!payload::guard_header_ok(*record_)) {
        // 零/坏 header = 未知（可能是 Native 的创建窗口、旧格式、或别人乱写）。fail closed：
        // 绝不复置/覆盖，绝不当成"没有不确定性"。
        error = L"the payload guard record has no usable header; treating this target as uncertain";
        ::SetEvent(event_);
        abandoned_ = true;
        return GuardAcquire::Unknown;
    }

    GuardSnapshot const snapshot = payload::guard_classify(*record_);
    seq_ = (*record_).seq;

    if (!was_abandoned) {
        // 正常持有（WAIT_OBJECT_0）。**Win32 mutex 是递归的**：同一条线程若还 owns 着上一次没有
        // 释放的租约，这里拿到的就是 WAIT_OBJECT_0 而不是 WAIT_ABANDONED —— 这正是"同一条线程
        // 把上一位 actor 留下的未知当成良性残留抹掉"的来源。所以这条路径上：
        //   * 粘性事件已置位 ⇒ 有未证实的入口，**绝不清、绝不当干净**；
        //   * 记录是 Pending* ⇒ 有人违反了写序（或内存被改过）。
        // 只有 `WAIT_ABANDONED` + 记录能证明干净那一条路径才允许清事件（见下面）。
        bool sticky = event_is_signaled(event_);
#if DESKFEED_TOOLS_GUARD_MUTATION == 3
        sticky = false;  // MUTATION(3)：把粘性未知当良性残留 —— 用例必须因此变红。
#endif
        if (snapshot == GuardSnapshot::Idle && !sticky) {
            // 干净释放的租约本来就该留下 false 事件，所以这里什么都不用清。
            return GuardAcquire::Acquired;
        }
        error = sticky
                    ? L"the sticky uncertainty event is set for this target; a lease acquired "
                      L"without an abandoned signal must never erase it"
                    : L"an entry record is pending on a lease that was released normally; treating "
                      L"this target as uncertain";
        ::SetEvent(event_);
        abandoned_ = true;
        return GuardAcquire::Unknown;
    }

    // WAIT_ABANDONED：上一位 actor 崩了。**在持锁状态下**按记录形状 + TID 事实判定。
    GuardTidProbe probe = GuardTidProbe::NotQueried;
    if (snapshot == GuardSnapshot::PendingTid) {
        probe = guard_probe_thread(target.process_id, (*record_).tid);
    }
    if (payload::guard_recovery(snapshot, probe) == GuardRecovery::ClearAndContinue) {
        ++seq_;
        payload::guard_make_idle(*record_, seq_);
        ::ResetEvent(event_);
        return GuardAcquire::Recovered;
    }

    error = L"the previous actor left an unprovable entry record; the sticky uncertainty stays set "
            L"and this action must not touch the target";
    ::SetEvent(event_);
    abandoned_ = true;
    return GuardAcquire::Unknown;
}

GuardSnapshot GuardLease::snapshot() const noexcept {
    if (!owned_by_current_thread() || record_ == nullptr) {
        return GuardSnapshot::Unknown;
    }
    return payload::guard_classify(*record_);
}

bool GuardLease::note_remote_call_pending(std::wstring& error) {
    error.clear();
    if (!held()) {
        error = L"the guard lease is not held by this thread";
        return false;
    }
    // 写序第一步：必须在 CreateRemoteThread **之前**落盘，否则"已创建、还没进模块"那段窗口
    // 就没有任何记录。
    ++seq_;
    payload::guard_make_pending_no_tid(*record_, seq_);
    pending_ = true;
    conclusive_ = false;
    return true;
}

bool GuardLease::note_remote_call_started(unsigned long tid, std::wstring& error) {
    error.clear();
    if (!held()) {
        error = L"the guard lease is not held by this thread";
        return false;
    }
    if (tid == 0) {
        error = L"refusing to record a zero thread id as a pending entry";
        return false;
    }
    ++seq_;
    payload::guard_make_pending_tid(*record_, tid, seq_);
    pending_ = true;
    conclusive_ = false;
    return true;
}

bool GuardLease::note_remote_call_settled(std::wstring& error) {
    error.clear();
    if (!held()) {
        error = L"the guard lease is not held by this thread";
        return false;
    }
    ++seq_;
    payload::guard_make_idle(*record_, seq_);
    pending_ = false;
    return true;
}

bool GuardLease::conclude_clean(std::wstring& error) {
    error.clear();
    if (!owned_by_current_thread()) {
        // mutex 所有权属于线程：不是本线程持有就既不能写记录也不能释放。
        error = L"the guard lease is not owned by this thread";
        return false;
    }
    if (abandoned_) {
        error = L"this lease already ended as unknown and cannot be concluded clean";
        return false;
    }
#if DESKFEED_TOOLS_GUARD_MUTATION == 1
    // MUTATION(1)：跳过释放谓词，直接往下走 —— 用例必须因此变红。
#else
    if (pending_) {
        error = L"there is an unresolved remote call; refusing to release the lease";
        return false;
    }
    if (!conclusive_) {
        error = L"the action is not conclusive; refusing to release the lease";
        return false;
    }
#endif

    ++seq_;
    payload::guard_make_idle(*record_, seq_);
    ::ResetEvent(event_);
    // 顺序（G1.4）：定论 → 清事件 → 释放。日志/清理必须排在释放之后，由调用方做；否则"卸载其实
    // 已经完成、只是收尾日志崩了"也会被记成不确定。
    if (::ReleaseMutex(mutex_) == FALSE) {
        // 跨线程释放会 ERROR_NOT_OWNER：**不能**当成已释放（否则租约握在一条没人管的线程上）。
        error = L"ReleaseMutex failed: " + deskfeed::last_error_text();
        abandoned_ = true;
        return false;
    }
    released_ = true;
    owner_thread_ = 0;
    return true;
}

void GuardLease::abandon_unknown() noexcept {
#if DESKFEED_TOOLS_GUARD_MUTATION == 2
    // MUTATION(2)：abandon 也去释放租约 —— 用例必须因此变红。
    if (mutex_ != nullptr && !released_) {
        ::ReleaseMutex(mutex_);
        released_ = true;
        owner_thread_ = 0;
        return;
    }
#endif
    if (mutex_ != nullptr && !released_) {
        if (event_ != nullptr) {
            ::SetEvent(event_);
        }
        abandoned_ = true;
    }
}

}  // namespace deskfeed::inject
