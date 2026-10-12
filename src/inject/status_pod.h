#pragma once

#include <cstddef>
#include <cstdint>

// 负载生命周期状态的**固定 POD**。布局的**唯一出处**是负载侧的 `src/payload/status_pod.h`
// （Native 所有，已随公共仓库一起提供）。这里只转发包含并起别名，**没有**第二份布局：
// 两份各自维护的结构定义是不可靠的，任何一处多一个字段，注入器与负载就会对同一段内存有两种
// 解释。头不在包含路径上时直接编译失败，绝不悄悄退回一份"看起来一样"的本地版本。
//
// 为什么需要它：只"模块表里有没有这个 DLL"回答不了"任务栏那块还在不在工作"。主进程正常收尾或
// 崩溃之后，负载会自己摘块、自己结束线程，但**模块仍然映射着**——要等注入器来卸载。此时同一
// scope 的新主进程启动、注入器只按"mapped"回答，新主进程就会跳过装载，任务栏上永远没有东西。
//
// ABI 形状（安全理由，详见负载侧那份头）：
//   * 调用方在目标进程里 `VirtualAllocEx` 一块定长缓冲，先写好 `size`/`abi`/`width`，再用
//     `CreateRemoteThread` 调 `DeskFeedGetStatus(void*)`；负载只写这块缓冲，调用方等远程线程
//     退出后 `ReadProcessMemory` 读回来。**没有指针跨越进程边界**。
//   * `size`/`abi` 不认就写回 `abi = 0`，调用方据此报"这个负载版本没有状态 ABI"。
//   * 144 字节由负载侧头里的 `width = 64` 显式钉死，不靠"猜对齐"。
#if defined(__has_include)
#  if __has_include("payload/status_pod.h")
#    include "payload/status_pod.h"
#    define DESKFEED_INJECT_HAS_COMMON_STATUS_POD 1
#  endif
#endif

#ifndef DESKFEED_INJECT_HAS_COMMON_STATUS_POD
#  error "注入器找不到负载侧 common ABI 头 payload/status_pod.h：状态 POD 布局不是工具侧的私有定义，必须由负载侧提供（工具不得自带副本）。"
#endif

namespace deskfeed::inject {

using deskfeed::payload::kPayloadScopeCapacity;
using deskfeed::payload::kPayloadStatusAbi;
using deskfeed::payload::kPayloadStatusExportName;
using deskfeed::payload::kPayloadStatusSize;
using deskfeed::payload::kPayloadStatusWidth;
using deskfeed::payload::PayloadStatusPod;
// 只读地把 POD 的 scope 取成字符数（在容量内截断）。它**只**能用在已经确认"串在容量内以 NUL
// 结尾"的 POD 上；校验路径必须自己逐字符检查（见 payload_status.cpp），不许拿这个截断值当结论。
using deskfeed::payload::pod_scope_length;

inline constexpr std::size_t kPayloadStatusPodBytes = deskfeed::payload::kPayloadStatusSize;

static_assert(sizeof(PayloadStatusPod) == kPayloadStatusPodBytes,
              "状态 POD 布局变了：负载与注入器必须对 144 字节达成一致");
static_assert(sizeof(PayloadStatusPod) % 8 == 0, "状态 POD 必须保持 8 字节对齐");

}  // namespace deskfeed::inject
