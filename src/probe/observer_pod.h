#pragma once

#include <cstddef>
#include <cstdint>

// 负载**回调阶段快照**（R3）与**钩子门状态**（R2）在探针这一侧的唯一入口。
//
// 布局的唯一出处是负载侧的 common ABI 头 `src/payload/probe_api.h`（MAIN 冻结）。这里只转发包含
// 并起别名，**不**自带第二份布局：两份各自维护的结构定义是不可靠的，任何一处多一个字段，探针与
// 负载就会对同一段内存有两种解释。头不在包含路径上时直接编译失败，而不是悄悄退回一份"看起来
// 一样"的本地版本。
//
// 历史：探针曾提议过一份 8×u32 的"计数 POD"（`HookPhasePod`，32 字节）。common ABI 定稿后它已被
// 取代，原因是它要求负载把计数**指给**探针（共享内存），而 common 选了**拉模型**：
//   * `DeskFeedProbeSetObserver(int)` 打开/关闭阶段记录；
//   * `DeskFeedProbeObserverStage(HookProbeStage*, unsigned)` 每次调用取一份**最后阶段快照**。
// 拉模型没有任何"回调已返回"的字段（`done` 之后负载的钩子只剩一次裸返回，不许再写字段、调 API、
// 跑析构），所以"帧已经弹掉"必须由宿主自己的 ack 观测来证明，而不是读负载的计数器。
#if defined(__has_include)
#  if __has_include("payload/probe_api.h")
#    include "payload/probe_api.h"
#    define DESKFEED_PROBE_HAS_COMMON_ABI 1
#  endif
#endif

#ifndef DESKFEED_PROBE_HAS_COMMON_ABI
#  error "探针找不到负载侧 common ABI 头 payload/probe_api.h：它不是探针的私有布局，必须由负载侧提供（探针不得自带一份副本）。"
#endif

namespace deskfeed::probe {

using deskfeed::payload::CompletionMutation;
using deskfeed::payload::HookObserverStage;
using deskfeed::payload::HookProbeStage;
using deskfeed::payload::HookProbeStatus;
using deskfeed::payload::kProbeStageSize;
using deskfeed::payload::kProbeStageVersion;
using deskfeed::payload::kProbeStatusSize;
using deskfeed::payload::kProbeStatusVersion;

static_assert(sizeof(HookProbeStage) == kProbeStageSize,
              "阶段 POD 布局变了：探针与负载必须对同一尺寸达成一致");
static_assert(sizeof(HookProbeStatus) == kProbeStatusSize,
              "钩子门状态 POD 布局变了：探针与负载必须对同一尺寸达成一致");

// "这一次快照表示负载正跑在自己的 `CallNextHookEx` 帧里"。
//
// 判据只有三个字段：进入过 CallNext（`CallNextEntered`）、这一次回调还没走完（`complete == 0`）、
// 而且确实观测到过一次回调（`sequence != 0`，否则 `stage` 的初值会被误读成"正在 CallNext"）。
// 快照与真实执行之间有轻微竞态，方向是"最多多算一次进入"——宁可让第三方钩子多泵一次（会被
// 记成硬失败证据并被人核查），也不能因为门太严而漏掉一次真的帧内泵消息。
inline bool stage_inside_callnext(HookProbeStage const& stage) {
    return stage.sequence != 0 && stage.complete == 0 &&
           stage.stage == static_cast<std::uint32_t>(HookObserverStage::CallNextEntered);
}

// 阶段名（只给日志用，不做任何判定）。
inline char const* hook_stage_name(std::uint32_t stage) {
    switch (static_cast<HookObserverStage>(stage)) {
        case HookObserverStage::None: return "none";
        case HookObserverStage::Enter: return "enter";
        case HookObserverStage::RecordRemoved: return "record-removed";
        case HookObserverStage::ExecutorDone: return "executor-done";
        case HookObserverStage::CallNextEntered: return "callnext-entered";
        case HookObserverStage::CallNextReturned: return "callnext-returned";
    }
    return "unknown";
}

}  // namespace deskfeed::probe
