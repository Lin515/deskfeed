#pragma once

// per-target 租约（mutex）+ 入口记录（named mapping）+ 粘性不确定事件（named event）。
//
// 解决的是 [final-lifecycle-review §7.1/§7.2] 的两条硬要求：
//   1. **同一次动作只有一个 actor**，而且释放租约必须有谓词（`knownRemoteCall == NONE &&
//      actionConclusive`）。异常/提前 return 一律**不释放**（线程退出 ⇒ mutex abandoned ⇒ 事实
//      留在内核对象里），绝不写"离开作用域自动释放"的 RAII。
//   2. 崩掉的 actor 不能把这个 explorer 变成"本会话永久不可卸载"：下一位 actor 在
//      `WAIT_ABANDONED` 时**持锁**读记录，只有记录能证明干净（Idle，或 PendingTid 且该 TID 已经
//      不在目标进程里）才清掉粘性事件继续；其余一律保持事件并立刻以未知收场。
//
// 记录布局、状态值、对象名、FNV、恢复判定全部来自唯一出处 `payload/guard_record.h`：本文件
// **不复制任何布局或常量**，只用别名转发 + `static_assert` 钉大小。
//
// 适用范围（诚实边界）：只对本协议版本的 cooperating 参与者成立（本仓库 deskfeed-inject.exe 与
// 负载 DLL）。第三方工具、调试器、手工 `FreeLibrary`、不拿租约的调用者、以及外部线程入口
// （DllGetClassObject / XAML 回调 / 探针入口）都不在这套门里。
#include <windows.h>

#include <cstdint>
#include <string>

#include "payload/guard_record.h"

namespace deskfeed::inject {

// 唯一出处的别名（工具侧不重复声明布局/常量）。
using GuardRecord = deskfeed::payload::GuardRecord;
using GuardSnapshot = deskfeed::payload::GuardSnapshot;
using GuardTidProbe = deskfeed::payload::GuardTidProbe;
using GuardRecovery = deskfeed::payload::GuardRecovery;

static_assert(sizeof(GuardRecord) == deskfeed::payload::kGuardRecordSize,
              "工具侧必须用载荷侧那一份 40 字节布局");

// 目标身份：实际 pid + **捕获到的**创建时间。PID 会被复用，所以任何"还是当初那个进程"的判断都
// 必须带创建时间，并且一次动作内只开一次进程句柄（见 remote.h 的 TargetProcess）。
struct GuardTarget {
    unsigned long process_id = 0;
    std::uint64_t creation_time = 0;
};

// 一次租约获取的结论。
enum class GuardAcquire {
    Acquired,   // 正常拿到（记录是 Idle，或本次是创建者并已按规约 init）：可以开始动作
    Recovered,  // 上一位 actor 崩了，但记录能证明干净：已清事件并把它重写回 Idle，继续
    Busy,       // 另一个 actor 正持有：不排队、不强抢、什么都不碰
    Unknown,    // 崩了且不可证：已置**粘性**事件，本次不得做任何 Stop/eject/Start 动作
    Error,      // 参数或系统调用失败：不得继续
};

// 只有这两个结论允许继续动作（用例直接咬这张表）。
inline bool guard_acquire_allows_action(GuardAcquire outcome) noexcept {
    return outcome == GuardAcquire::Acquired || outcome == GuardAcquire::Recovered;
}

// 纯映射：把"记录形状 + TID 事实"变成获取结论（`guard_recovery` 的唯一出处仍在载荷头里）。
inline GuardAcquire guard_acquire_from_recovery(GuardSnapshot snapshot, GuardTidProbe probe) noexcept {
    return deskfeed::payload::guard_recovery(snapshot, probe) == GuardRecovery::ClearAndContinue
               ? GuardAcquire::Recovered
               : GuardAcquire::Unknown;
}

// 目标进程里某个 TID 是否还在（官方 Toolhelp，仅在 actor 侧使用；Native 不做枚举）。
// `tid == 0` 或超过 32 位一律 `LookupFailed`（不可证），绝不当作"不存在"。
GuardTidProbe guard_probe_thread(unsigned long process_id, std::uint64_t tid);

// 取目标进程的创建时间（用于拼对象名与身份校验）。失败返回 false。
bool guard_target_identity(unsigned long process_id, GuardTarget& out, std::wstring& error);

// 一次动作持有的租约。**线程亲和**：mutex 所有权属于线程，所以一个实例必须在同一条线程上
// 获取、写记录、收尾。默认收尾是"abandon（不释放）"，不是"释放"。
//
// 使用契约（违反它整套结论就不成立）：
//   * `acquire()` 每个实例只调用一次。**abandon 过的实例必须原样退场**，不得复用：它的三份句柄
//     就是"未知"这条事实的载体，而 Win32 mutex 是**同线程递归**的——同一条线程再 acquire 会拿到
//     `WAIT_OBJECT_0`（不是 `WAIT_ABANDONED`），于是上一位 actor 留下的未知会被它自己抹平。
//   * 结论是 `Unknown`，或调用过 `abandon_unknown()` 之后，**本线程必须结束**（内核随即把 mutex
//     标成 abandoned，事实留给下一位 actor）。不要把这条线程继续用于下一轮或下一个动作：
//     `--watch` 的每一轮要么 `conclude_clean()`，要么让该轮自己的 actor 线程结束。
//   * 只有 `WAIT_ABANDONED` + 记录能证明干净时才可以清粘性事件；正常持有（`WAIT_OBJECT_0`）时
//     看到粘性事件**绝不清**，一律按未知收场。
class GuardLease {
public:
    GuardLease() = default;
    ~GuardLease();
    GuardLease(GuardLease const&) = delete;
    GuardLease& operator=(GuardLease const&) = delete;

    // 创建/打开三个命名对象并（必要时）初始化记录，然后拿租约。
    GuardAcquire acquire(GuardTarget const& target, std::wstring& error);

    bool held() const noexcept {
        return mutex_ != nullptr && !released_ && !abandoned_ &&
               owner_thread_ == ::GetCurrentThreadId();
    }
    bool owned_by_current_thread() const noexcept {
        return mutex_ != nullptr && !released_ && owner_thread_ == ::GetCurrentThreadId();
    }
    GuardTarget const& target() const noexcept { return target_; }
    // 读当前记录形状（要求本线程仍持有租约；不持有时返回 Unknown）。
    GuardSnapshot snapshot() const noexcept;

    // 记录写序（每一步都要求本线程持有租约；失败会被如实返回，不静默降级）。
    //   CreateRemoteThread **之前**：PendingNoTid
    bool note_remote_call_pending(std::wstring& error);
    //   拿到真实 TID 之后：PendingTid{tid}
    bool note_remote_call_started(unsigned long tid, std::wstring& error);
    //   远程调用**真实返回/真实目标死亡**之后：Idle
    bool note_remote_call_settled(std::wstring& error);

    // 本次动作是否已经定论（所有查询都可信、没有任何未决远程调用）。由调用方在每一步之后设置。
    void note_action_conclusive(bool conclusive) noexcept { conclusive_ = conclusive; }
    bool action_conclusive() const noexcept { return conclusive_; }
    bool has_pending_remote_call() const noexcept { return pending_; }

    // **唯一**的 `ReleaseMutex` 调用点：只有谓词成立才释放。
    // 谓词不成立时：不释放、保持事实，返回 false。
    bool conclude_clean(std::wstring& error);

    // 明确的"未知"收尾：置粘性事件，**不**释放租约（线程退出 ⇒ abandoned ⇒ 事实保留）。
    void abandon_unknown() noexcept;

private:
    void close_all_handles() noexcept;

    GuardTarget target_{};
    HANDLE mutex_ = nullptr;
    HANDLE event_ = nullptr;
    HANDLE mapping_ = nullptr;
    GuardRecord* record_ = nullptr;
    DWORD owner_thread_ = 0;
    bool released_ = false;
    bool abandoned_ = false;
    bool pending_ = false;
    bool conclusive_ = false;
    std::uint64_t seq_ = 0;
};

}  // namespace deskfeed::inject
