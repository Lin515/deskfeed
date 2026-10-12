#pragma once

#include "probe/host.h"

#include <string>
#include <vector>

// 探针宿主的 **feature gate**：把"这次命令行开了哪些功能/对抗"换算成"必须能在负载 DLL 里
// 解析到哪些导出"，再与实际的解析结果比对。
//
// 为什么要有它：自测宿主的每一种模式都靠负载导出的入口驱动。导出缺失时若只是"日志里说一句
// 就跳过这一条"，命令行的退出码仍然是 0 —— 验收脚本会把"没跑"读成"跑过了、没问题"。所以这里
// 是一个**纯决策**：缺任何一条必需导出就是 BLOCKED（`kHostExitBlocked`），一条也不跑，绝不假绿。
//
// 这一层不碰 DLL、不做 I/O、不打日志：宿主只按它给出的清单逐条打印并退出，离线用例按它逐条
// 钉住"哪个功能需要哪个符号"。它是纯函数，所以"缺导出必须 BLOCKED"这件事本身可以被测。
namespace deskfeed::probe {

// 负载 DLL 里按名字解析到的探针导出（true = 解析到了）。
struct ExportPresence {
    // `--mock` 结构自测的基线。
    bool start = false;      // DeskFeedProbeStart
    bool sites = false;      // DeskFeedProbeSites
    bool watch = false;      // DeskFeedProbeWatch
    bool run = false;        // DeskFeedProbeRun
    bool set_host = false;   // DeskFeedProbeSetHost
    bool stop = false;       // DeskFeedProbeStop
    // 收尾与对抗。
    bool set_slow_apply = false;           // DeskFeedProbeSetSlowApply
    bool post_apply = false;               // DeskFeedProbePostApply
    bool retire = false;                   // DeskFeedProbeRetire
    bool hook = false;                     // DeskFeedProbeHook（导入表自检）
    bool teardown = false;                 // DeskFeedProbeTeardown
    bool dangling = false;                 // DeskFeedProbeDangling
    // 探针 ABI（`src/payload/probe_api.h`，MAIN 冻结）。
    bool hook_status = false;              // R2 DeskFeedProbeHookStatus
    bool set_observer = false;             // R3 DeskFeedProbeSetObserver
    bool observer_stage = false;           // R3 DeskFeedProbeObserverStage
    bool set_install_delay = false;        // R1 DeskFeedProbeSetInstallDelay
    bool set_completion_mutation = false;  // R4 DeskFeedProbeSetCompletionMutation
    bool set_tray_window = false;          // R5 DeskFeedProbeSetTrayWindow
    // 生产入口与状态 ABI。
    bool feed_start = false;  // DeskFeedStart
    bool feed_stop = false;   // DeskFeedStop
    bool get_status = false;  // DeskFeedGetStatus（144 字节状态 POD）
};

// 一条缺失的接口：哪个功能需要哪个符号。
struct GateMiss {
    std::wstring feature;   // 命令行上开着的那个功能（面向人）
    wchar_t const* symbol;  // 它需要的负载导出名
};

struct GateDecision {
    bool blocked = false;
    std::vector<GateMiss> missing;  // 按功能与符号逐条列出，绝不只报一个总数
};

namespace gate_detail {

inline void require(std::vector<GateMiss>& out, bool present, wchar_t const* feature,
                    wchar_t const* symbol) {
    if (!present) {
        out.push_back(GateMiss{feature, symbol});
    }
}

}  // namespace gate_detail

// 纯决策：由命令行选项算出这次要跑的每一条所需导出清单，与解析结果比对。
inline GateDecision gate_decision(HostOptions const& options, ExportPresence const& presence) {
    GateDecision decision;
    std::vector<GateMiss>& miss = decision.missing;

    if (options.feed) {
        // 生产入口：负载自己 attach 邮箱、自己经真实钩子编组渲染，收尾用真实 Stop。
        wchar_t const* const feed = L"--feed";
        gate_detail::require(miss, presence.set_tray_window, feed, L"DeskFeedProbeSetTrayWindow");
        gate_detail::require(miss, presence.feed_start, feed, L"DeskFeedStart");
        gate_detail::require(miss, presence.feed_stop, feed, L"DeskFeedStop");
        // 144 字节状态 POD 是 feed 的收尾判据（线程句柄、unload_safe、dangling）：缺了就只能
        // "跑完但无从判定"，那必须 BLOCKED，不能拿"stop 返回 1"单独当结论。
        gate_detail::require(miss, presence.get_status, feed, L"DeskFeedGetStatus");
        gate_detail::require(miss, presence.dangling, feed, L"DeskFeedProbeDangling");
        if (options.slow_apply_ms != 0) {
            gate_detail::require(miss, presence.set_slow_apply, L"--slow-apply-ms",
                                 L"DeskFeedProbeSetSlowApply");
        }
    } else {
        wchar_t const* const mock = L"--mock";
        gate_detail::require(miss, presence.start, mock, L"DeskFeedProbeStart");
        gate_detail::require(miss, presence.sites, mock, L"DeskFeedProbeSites");
        gate_detail::require(miss, presence.watch, mock, L"DeskFeedProbeWatch");
        gate_detail::require(miss, presence.run, mock, L"DeskFeedProbeRun");
        gate_detail::require(miss, presence.set_host, mock, L"DeskFeedProbeSetHost");
        gate_detail::require(miss, presence.stop, mock, L"DeskFeedProbeStop");
        if (options.slow_apply_ms != 0) {
            gate_detail::require(miss, presence.set_slow_apply, L"--slow-apply-ms",
                                 L"DeskFeedProbeSetSlowApply");
        }
    }

    if (options.teardown && !options.feed) {
        gate_detail::require(miss, presence.teardown, L"--teardown", L"DeskFeedProbeTeardown");
        gate_detail::require(miss, presence.dangling, L"--teardown", L"DeskFeedProbeDangling");
    }
    if (options.balance_check) {
        gate_detail::require(miss, presence.stop, L"--balance-check", L"DeskFeedProbeStop");
    }
    if (options.hook) {
        gate_detail::require(miss, presence.hook, L"--hook", L"DeskFeedProbeHook");
    }

    // 对抗序列（第三方钩子泵、安装竞态、提前 done）共用同一组导出：投递、收尾、阶段观测、
    // 钩子门状态、悬挂判定。
    bool const ordering = options.third_party || options.install_delay_ms != 0;
    if (ordering) {
        wchar_t const* const feature = options.mutation_early_done  ? L"--mutation-early-done"
                                       : options.install_delay_ms != 0 ? L"--install-race"
                                                                       : L"--third-party-hook-pump";
        gate_detail::require(miss, presence.post_apply, feature, L"DeskFeedProbePostApply");
        gate_detail::require(miss, presence.retire, feature, L"DeskFeedProbeRetire");
        gate_detail::require(miss, presence.set_observer, feature, L"DeskFeedProbeSetObserver");
        gate_detail::require(miss, presence.observer_stage, feature, L"DeskFeedProbeObserverStage");
        gate_detail::require(miss, presence.hook_status, feature, L"DeskFeedProbeHookStatus");
        gate_detail::require(miss, presence.dangling, feature, L"DeskFeedProbeDangling");
    }
    if (options.install_delay_ms != 0) {
        gate_detail::require(miss, presence.set_install_delay, L"--install-race",
                             L"DeskFeedProbeSetInstallDelay");
    }
    if (options.mutation_early_done) {
        gate_detail::require(miss, presence.set_completion_mutation, L"--mutation-early-done",
                             L"DeskFeedProbeSetCompletionMutation");
    }

    // "挂住 UI / 慢执行 + 短超时 stop 必须未退休"：mock 走 post_apply/retire，feed 走 feed_stop
    // （后者已在上面的 feed 分支里强制）。
    if (!options.feed && (options.hang_ui_ms != 0 || options.slow_apply_ms != 0)) {
        wchar_t const* const feature = L"--hang-ui-ms/--slow-apply-ms";
        gate_detail::require(miss, presence.post_apply, feature, L"DeskFeedProbePostApply");
        gate_detail::require(miss, presence.retire, feature, L"DeskFeedProbeRetire");
        gate_detail::require(miss, presence.dangling, feature, L"DeskFeedProbeDangling");
    }

    decision.blocked = !decision.missing.empty();
    return decision;
}

}  // namespace deskfeed::probe
