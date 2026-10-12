#pragma once

#include <cstddef>
#include <cstdint>

// 负载 `hook_post` 的**阶段计数 POD**（提议版，等负载作者在 `src/payload/` 冻结同一份布局）。
//
// 为什么需要它：第三方钩子必须能判断"我现在是不是正跑在负载那一次 `CallNextHookEx` 里"。
// 只有这种时候它才允许泵消息——否则一条与本次编组无关的消息检索也会被当成对抗场景，得到假失败。
//
// 严格约束（MAIN 已裁定）：POD 里**只能有 `done` 之前写下的阶段计数**。`done` 之后负载的钩子
// 过程必须只剩一次裸返回：不许再写日志、不许再调 API、不许再有析构。所以这里**没有**任何
// "回调已返回"的字段——"帧真的弹掉了"由宿主自己的 ack 观测（见 probe_hooks）来证明。
namespace deskfeed::probe {

inline constexpr std::uint32_t kHookPhaseAbi = 1;
inline constexpr std::size_t kHookPhasePodBytes = 32;  // 8 个 u32，8 字节对齐

#if defined(__has_include)
#  if __has_include("payload/probe_observer.h")
#    define DESKFEED_PROBE_USES_PAYLOAD_OBSERVER 1
#  endif
#endif

}  // namespace deskfeed::probe

#ifdef DESKFEED_PROBE_USES_PAYLOAD_OBSERVER

#include "payload/probe_observer.h"

namespace deskfeed::probe {
using deskfeed::payload::HookPhasePod;
}  // namespace deskfeed::probe

#else

namespace deskfeed::probe {

struct HookPhasePod {
    std::uint32_t size = 0;               // 调用方写 sizeof(HookPhasePod)
    std::uint32_t abi = 0;                // 调用方写 kHookPhaseAbi
    std::uint32_t n_hook_entered = 0;     // 回调进入
    std::uint32_t n_record_removed = 0;   // 记录自摘成功
    std::uint32_t n_executor_done = 0;    // 执行体与结果发布做完
    std::uint32_t n_callnext_entered = 0; // 进入 CallNextHookEx 之前
    std::uint32_t n_callnext_returned = 0;// CallNextHookEx 返回之后（仍在 done 之前）
    std::uint32_t reserved = 0;
};

}  // namespace deskfeed::probe

#endif  // DESKFEED_PROBE_USES_PAYLOAD_OBSERVER

namespace deskfeed::probe {

static_assert(sizeof(HookPhasePod) == kHookPhasePodBytes,
              "the hook phase POD layout changed: producer and probe must agree on 32 bytes");

// 负载是否正处在自己的 `CallNextHookEx` 里（进入比返回多一次）。字段是原子计数，读取有轻微
// 竞态但状态只会在 entered/returned 之间，误判方向是"多算一次入口"，不会把对抗漏掉。
inline bool phase_inside_callnext(HookPhasePod const& pod) {
    return pod.n_callnext_entered > pod.n_callnext_returned;
}

}  // namespace deskfeed::probe
