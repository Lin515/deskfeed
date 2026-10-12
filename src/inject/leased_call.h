#pragma once

// 一次"受租约保护的远程调用"的公共形状。
//
// 为什么必须有它：写序（`PendingNoTid` → `CreateRemoteThread` → `PendingTid` → 真实 signal →
// `Idle`）与 parking（等到**真实**线程退出或**捕获的**目标进程死亡）必须落在**同一条**代码路径上，
// 否则总有一条分支会退回"超时之后当作干净"。注入器的五个调用点（`LoadLibraryW` / `Start` /
// `GetStatus` / `Stop` / `FreeLibrary`）与状态查询的 `GetStatus` 都只走这里：
//
//   1. `note_remote_call_pending()` —— 必须在 `CreateRemoteThread` **之前**（由本函数保证次序）；
//   2. `issue()` —— 在**捕获的**目标句柄上真正建线程（`issue_*_on`）；
//   3. 拿到真实 TID → `note_remote_call_started(tid)`；
//   4. `park_remote_call()` —— 无界等到真实 signal / 真实目标死亡 / 打断；
//   5. 只有 `ThreadSignaled` 才 `note_remote_call_settled()`（此时记录才回 `Idle`）。
//
// **不**属于本函数的义务（故意留出来，绝不替调用方做决定）：
//   * 未决调用的线程句柄**不关闭**、租约**不释放**：那是"有一次入口调用没回来"这条事实的载体，
//     由调用方交给 `abandon_unknown()` 且让本线程结束；
//   * 远程缓冲的释放由调用方按 `may_release_remote_buffer(call.thread_created, call.signaled)`
//     判定（`Completed` 之后才允许释放）。
#include <functional>
#include <string>
#include <utility>

#include "inject/guard.h"
#include "inject/remote.h"

namespace deskfeed::inject {

// 一次受租约保护的远程调用的结论。除 `Completed` / `NeverExecuted` 外，**记录保持未决**：
// 调用方不得写回 `Idle`，也不得释放租约。
enum class LeaseCallOutcome {
    Completed,      // 线程真实退出（含 parking 观察到的迟到完成）：exit_code 有效，记录已回 Idle
    NeverExecuted,  // CreateRemoteThread 从未成功：本进程可证的实际结论（也已回 Idle，无入口在飞）
    TargetDied,     // 捕获的目标进程消失：模块随进程消失，**不等于**已卸载；记录保持未决
    Interrupted,    // 打断事件置位：结论未知；记录保持未决
    Error,          // 参数/系统调用失败（含写不回记录）：结论未知；记录保持未决
};

struct LeaseCall {
    LeaseCallOutcome outcome = LeaseCallOutcome::Error;
    RemoteCall call;
    ParkResult parked = ParkResult::NotPending;
    std::wstring error;

    bool completed() const noexcept { return outcome == LeaseCallOutcome::Completed; }
    bool actually_concluded() const noexcept {
        return outcome == LeaseCallOutcome::Completed || outcome == LeaseCallOutcome::NeverExecuted;
    }
};

// 在**已经持有租约的同一条线程**上发起并等待一次远程调用。`issue` 只能在这里面执行真正的
// `CreateRemoteThread`（用捕获的句柄）。`interrupt_event` 非空时置位会让本次调用以 `Interrupted`
// 收场（结论仍然未知，句柄与记录一律保留）。
inline LeaseCall run_leased_call(GuardLease& lease, TargetProcess const& target, DWORD tick_ms,
                                 std::function<RemoteCall()> const& issue,
                                 HANDLE interrupt_event = nullptr) {
    LeaseCall out;
    std::wstring error;

    // 写序第一步：必须在 CreateRemoteThread **之前**落盘，否则"已创建、还没进模块"那段窗口就没有
    // 任何记录。
    if (!lease.note_remote_call_pending(error)) {
        out.outcome = LeaseCallOutcome::Error;
        out.error = error.empty() ? L"cannot record the pending remote call" : error;
        return out;
    }

    out.call = issue();
    if (!out.call.thread_created) {
        // 线程从未执行：这是本进程可证的**实际**结论（与"超时未决"是两件事）。写回 Idle 之后
        // 租约仍然有效，本次动作可以继续。
        std::wstring settle_error;
        if (!lease.note_remote_call_settled(settle_error)) {
            out.outcome = LeaseCallOutcome::Error;
            out.error = settle_error;
            return out;
        }
        out.outcome = LeaseCallOutcome::NeverExecuted;
        out.error = out.call.error;
        return out;
    }

    if (!lease.note_remote_call_started(out.call.thread_id, error)) {
        out.outcome = LeaseCallOutcome::Error;
        out.error = error;
        return out;
    }

    out.parked = park_remote_call(out.call, target.process, tick_ms, interrupt_event);
    switch (out.parked) {
        case ParkResult::ThreadSignaled:
            if (!lease.note_remote_call_settled(error)) {
                out.outcome = LeaseCallOutcome::Error;
                out.error = error;
                return out;
            }
            out.outcome = LeaseCallOutcome::Completed;
            return out;
        case ParkResult::NotPending:
            // 线程在 park 之前就已经 signal（`park_remote_call` 对已退出的调用直接收尾并返回
            // NotPending）。只有确实 signaled 才算实际结论；否则一律按未知保留。
            if (out.call.signaled) {
                if (!lease.note_remote_call_settled(error)) {
                    out.outcome = LeaseCallOutcome::Error;
                    out.error = error;
                    return out;
                }
                out.outcome = LeaseCallOutcome::Completed;
                return out;
            }
            out.outcome = LeaseCallOutcome::Error;
            out.error = L"the remote call has no conclusion and nothing to park on";
            return out;
        case ParkResult::TargetDied:
            out.outcome = LeaseCallOutcome::TargetDied;
            out.error = out.call.error;
            return out;
        case ParkResult::Interrupted:
            out.outcome = LeaseCallOutcome::Interrupted;
            out.error = L"the wait was interrupted; the entry thread has no conclusion yet";
            return out;
        case ParkResult::Error:
        default:
            out.outcome = LeaseCallOutcome::Error;
            out.error = out.call.error.empty() ? L"parking the remote call failed" : out.call.error;
            return out;
    }
}

}  // namespace deskfeed::inject
