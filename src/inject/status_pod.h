#pragma once

#include <cstddef>
#include <cstdint>

// 负载生命周期状态的**固定 POD**。
//
// 布局只有**一个**出处：`src/payload/status_pod.h`（负载作者所有）。这个头只是把它引进来并给
// 注入器这一侧一个稳定的别名——两份各自维护的结构定义是不可靠的：任何一处多一个字段，两侧就会
// 对同一段内存有两种解释。定义落地之前，这里保留同一份布局作为**临时**后备（`__has_include`
// 一旦能找到 `payload/status_pod.h` 就改用它），并用 `static_assert` 把约定的大小钉住。
//
// 为什么需要它：只"模块表里有没有这个 DLL"回答不了"任务栏那块还在不在工作"。主进程正常收尾或
// 崩溃之后，负载会自己摘块、自己结束线程，但**模块仍然映射着**——要等注入器来卸载。此时同一
// scope 的新主进程启动、注入器只按"mapped"回答 `--status` 0，新主进程就会跳过装载，任务栏上
// 永远没有东西。
//
// 所以状态必须能区分 mapped / active / retained，而"active"要能证明三件事：
//   ① 负载线程还活着（不是 `running=false` 这种标志位，而是线程句柄没触发）；
//   ② 它 attach 到了**这个 scope** 的显示邮箱，并且那个邮箱当前的生产者还活着、还没收尾；
//   ③ 它报的 scope 就是命令行的 scope。
//
// ABI 形状（安全理由）：
//   * 调用方（注入器）在目标进程里 `VirtualAllocEx` 一块定长缓冲，先把 `size`/`abi` 与全部字段
//     清零写进去，再用 `CreateRemoteThread` 调 `DeskFeedGetStatus(void* pod)`；远程函数只写这块
//     缓冲，调用方等远程线程退出后 `ReadProcessMemory` 读回来。**没有指针跨越进程边界**，
//     也没有任何字符串在两侧传递。
//   * `size` 与 `abi` 由调用方写入；负载不认这两个值时把 `abi` 写 0 并立刻返回，调用方据此
//     报"这个负载版本没有状态 ABI"，而不是拿一张半张 POD 当结论。
//   * 结构只含定长整型与一个定长宽字符 scope 串，两侧用同一份头编译，布局不靠"约定"。
namespace deskfeed::inject {

inline constexpr std::uint32_t kPayloadStatusAbi = 1;
inline constexpr std::size_t kPayloadScopeCapacity = 36;  // 32 个字符 + 结尾 + 余量
inline constexpr wchar_t kPayloadStatusExportName[] = L"DeskFeedGetStatus";

// 约定的固定大小：14 个 u32（含 size/abi 两个头字段）+ 2 个 u64 + 36 个 wchar_t = 144 字节。
inline constexpr std::size_t kPayloadStatusPodBytes = 144;

}  // namespace deskfeed::inject

#if defined(__has_include)
#  if __has_include("payload/status_pod.h")
#    define DESKFEED_INJECT_USES_PAYLOAD_STATUS_POD 1
#  endif
#endif

#ifdef DESKFEED_INJECT_USES_PAYLOAD_STATUS_POD

// 负载侧那一份是唯一出处；这里只引入并起别名。名字必须与 `src/payload/status_pod.h` 完全一致。
#include "payload/status_pod.h"

namespace deskfeed::inject {
using deskfeed::payload::PayloadStatusPod;
}  // namespace deskfeed::inject

#else  // !DESKFEED_INJECT_USES_PAYLOAD_STATUS_POD

namespace deskfeed::inject {

// 负载的 `DeskFeedGetStatus` 把这块 POD 写回来（临时后备定义，等 payload 侧那份头落地）。
struct PayloadStatusPod {
    std::uint32_t size = 0;               // 调用方写 sizeof(PayloadStatusPod)
    std::uint32_t abi = 0;                // 调用方写 kPayloadStatusAbi；不认就写回 0
    std::uint32_t started = 0;            // DeskFeedStart 已经接受过（g_started）
    std::uint32_t thread_alive = 0;       // 负载线程句柄**未触发**（不是标志位）
    std::uint32_t unload_safe = 0;        // payload_unload_safe()：1 = 门禁全过、可卸载
    std::uint32_t dangling = 0;           // hook_post_dangling()：有未证明安全的悬挂
    std::uint32_t mailbox_attached = 0;   // DisplayFrameConsumer::attach 成功
    std::uint32_t producer_alive = 0;     // consumer.producer_running()
    std::uint32_t producer_pid_low = 0;   // 邮箱帧头里的生产者 PID（低 32 位）
    std::uint32_t producer_pid_high = 0;  // 高 32 位（本工具里恒为 0）
    std::uint32_t frame_generation = 0;   // 最近一次成功发布/读到的帧代次（低 32 位）
    std::uint32_t feed_failures = 0;      // 连续"生产者活着但读不出帧"的轮数
    std::uint32_t xaml_block_present = 0; // 读数块当前在不在树里
    std::uint32_t scope_length = 0;       // scope 的字符数（不含结尾）
    std::uint64_t producer_start_time = 0;  // 生产者的进程创建时间（100ns）
    std::uint64_t reserved = 0;
    wchar_t scope[kPayloadScopeCapacity]{};  // NUL 结尾；空串 = 生产实例
};

}  // namespace deskfeed::inject

#endif  // DESKFEED_INJECT_USES_PAYLOAD_STATUS_POD

namespace deskfeed::inject {

static_assert(sizeof(PayloadStatusPod) == kPayloadStatusPodBytes,
              "the status POD layout changed: the payload and the injector must agree on 144 bytes");
static_assert(sizeof(PayloadStatusPod) % 8 == 0, "状态 POD 必须保持 8 字节对齐");

// 只读地把 POD 的 scope 取成一个 std::wstring（截断在容量内，不越界）。
inline std::size_t pod_scope_length(PayloadStatusPod const& pod) {
    std::size_t length = 0;
    while (length + 1 < kPayloadScopeCapacity && pod.scope[length] != L'\0') {
        ++length;
    }
    return length;
}

}  // namespace deskfeed::inject
