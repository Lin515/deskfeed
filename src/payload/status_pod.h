#pragma once

// 负载生命周期状态的**固定 POD**（注入器 `--status` 与自测探针共用）。
//
// 为什么需要它：只"模块表里有没有这个 DLL"回答不了"任务栏那块还在不在工作"。主进程正常收尾或
// 崩溃之后，负载会自己摘块、自己结束线程，但**模块仍然映射着**——要等注入器来卸载。此时同一
// scope 的新主进程启动、注入器只按"mapped"回答，新主进程就会跳过装载，任务栏上永远没有东西。
//
// 所以状态要能区分 mapped / active / retained，而 "active" 要能证明三件事：
//   1. 负载线程还活着——判据是**线程句柄没触发**，不是 `running=false` 这种标志位；
//   2. 它 attach 到了**这个 scope** 的显示邮箱，并且那个邮箱当前的生产者还活着；
//   3. 它报的 scope 就是命令行的 scope。
//
// ABI 形状（安全理由）：
//   * 调用方在目标进程里 `VirtualAllocEx` 一块定长缓冲，先写好 `size` 与 `abi`，再用
//     `CreateRemoteThread` 调 `DeskFeedGetStatus(void*)`；负载**只写这块缓冲**，调用方等远程线程
//     退出后 `ReadProcessMemory` 读回来。**没有指针跨越进程边界**，也没有任何字符串两侧传递。
//   * `size` / `abi` 不认就写回 `abi = 0` 并立刻返回，调用方据此报"这个负载版本没有状态 ABI"，
//     而不是拿一张半张 POD 当结论。
//   * 结构只含定长整型与一个定长宽字符 scope 串。这里**显式写出 `width`**，让 32 位调用方也能
//     在编译期算出 144 字节这个 ABI 尺寸，而不是靠"猜对齐"。
//
// 这份头是**唯一出处**：工具侧不要再复制一份布局，转发包含/别名这一份即可。
#include <cstddef>
#include <cstdint>

namespace deskfeed::payload {

inline constexpr std::uint32_t kPayloadStatusAbi = 1;
inline constexpr std::size_t kPayloadScopeCapacity = 36;  // 32 个字符 + 结尾 + 余量
inline constexpr std::uint32_t kPayloadStatusWidth = 64;
// 这个 ABI 就是按 64 位定死的：14 个 u32 + 2 个 u64 + 36 个 wchar_t（wchar_t 在 ABI 里算 2 字节）。
inline constexpr std::uint32_t kPayloadStatusSize = 144;
inline constexpr wchar_t kPayloadStatusExportName[] = L"DeskFeedGetStatus";

#pragma pack(push, 8)
struct PayloadStatusPod {
    std::uint32_t size = 0;                // 调用方写 sizeof(PayloadStatusPod)
    std::uint32_t abi = 0;                 // 调用方写 kPayloadStatusAbi；不认就写回 0
    std::uint32_t width = kPayloadStatusWidth;  // 固定 64：把 ABI 尺寸钉死，不靠指针宽度
    std::uint32_t started = 0;             // DeskFeedStart 已经接受过
    std::uint32_t thread_alive = 0;        // 负载线程句柄**未触发**（不是标志位）
    std::uint32_t unload_safe = 0;         // payload_unload_safe()：1 = 门禁全过、可卸载
    std::uint32_t dangling = 0;            // hook_post_dangling()：有未证明安全的悬挂
    std::uint32_t mailbox_attached = 0;    // DisplayFrameConsumer::attach 成功
    std::uint32_t producer_alive = 0;      // consumer.producer_running()
    std::uint32_t producer_pid_low = 0;    // 邮箱帧头里的生产者 PID（低 32 位）
    std::uint32_t producer_pid_high = 0;   // 高 32 位
    std::uint32_t frame_generation = 0;    // 最近一次成功发布/读到的帧代次（低 32 位）
    std::uint32_t feed_failures = 0;       // 连续"生产者活着但读不出帧"的轮数
    std::uint32_t xaml_block_present = 0;  // 读数块当前在不在树里
    std::uint64_t producer_start_time = 0;  // 生产者的进程创建时间（100ns）
    std::uint64_t reserved = 0;
    wchar_t scope[kPayloadScopeCapacity]{};  // NUL 结尾；空串 = 生产实例
};
#pragma pack(pop)

static_assert(sizeof(PayloadStatusPod) == kPayloadStatusSize,
              "PayloadStatusPod 必须是 144 字节的固定 POD");
static_assert(sizeof(wchar_t) == 2, "这个 ABI 假定 Windows 的 wchar_t 是 2 字节");

// 只读地把 POD 的 scope 取成字符数（截断在容量内，不越界）。
inline std::size_t pod_scope_length(PayloadStatusPod const& pod) {
    std::size_t length = 0;
    while (length + 1 < kPayloadScopeCapacity && pod.scope[length] != L'\0') {
        ++length;
    }
    return length;
}

}  // namespace deskfeed::payload

// 负载导出：把当前状态写进调用方给的 POD（见上面的 ABI 说明）。
// 返回值：1 = 已按当前 ABI 填好；0 = 参数/ABI 不认（此时 `abi` 被写回 0）。
// 实现见 src/payload/dllmain.cpp。这里不声明成带 dllexport 的形式：探针与注入器一律按名字
// `GetProcAddress`，不链接负载。
