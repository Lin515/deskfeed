#pragma once

// 自测探针（`deskfeed-probe.exe` / `tools_tests`）与负载之间**唯一**的共享接口。
//
// 这里只有固定大小的 POD 与函数声明：探针把这块内存写干净再交进来，负载只做受控的读取与填充，
// **不**输出任何 C++ 对象、指针、句柄或字符串。这样探针侧不需要（也不应该）链接负载的内部头，
// 也就不会因为内部结构变化而悄悄错位。
//
// 所有探针开关都是"自测宿主显式配置"的：生产路径（`DeskFeedStart`）启动时会先把它们复位，避免
// 任何人误以为可以靠它们改变生产行为。探针不得在注入真实 explorer 之后调用这些入口。
#include <cstdint>

#include <windows.h>

namespace deskfeed::payload {

inline constexpr std::uint32_t kProbeStatusSize = 40;      // 字节
inline constexpr std::uint32_t kProbeStatusVersion = 1;
inline constexpr std::uint32_t kProbeStageSize = 28;       // 字节
inline constexpr std::uint32_t kProbeStageVersion = 1;

// 探针阶段（只用于自测宿主那份记录，见 hook_post.cpp 的注释）。
enum class HookObserverStage : std::uint32_t {
    None = 0,
    Enter = 1,            // 进入回调
    RecordRemoved = 2,    // 记录已被本回调摘掉
    ExecutorDone = 3,     // 执行体与结果发布都跑完
    CallNextEntered = 4,  // 即将调用 CallNextHookEx
    CallNextReturned = 5, // CallNextHookEx 已返回（第三方泵已跑完）
};

// 完成事件的变异模式（**只**给探针用；正常与变异两条路径都必须能被观测区分）。
enum class CompletionMutation : std::uint32_t {
    Normal = 0,      // 完成事件在 CallNextHookEx 返回之后才置位
    EarlyBeforeCallNext = 1,  // 故意提前到 CallNextHookEx 之前，用来证明验收能抓住这类错误
};

// `DeskFeedProbeHookStatus` 的输出。字段顺序即 ABI，只在 version 变化时改。
#pragma pack(push, 4)
struct HookProbeStatus {
    std::uint32_t size;                 // = kProbeStatusSize
    std::uint32_t version;              // = kProbeStatusVersion
    std::uint32_t hook_installed;       // 链上还有我们的钩子记录
    std::uint32_t retired;              // 记录已被回调摘掉
    std::uint32_t still_installed;      // 摘钩失败过（UnhookWindowsHookEx 在回调里返回失败）
    std::uint32_t in_flight;            // 观测到的、正在回调里执行的次数
    std::uint32_t pending;              // 有一次已发布但还没被回调取走的动作
    std::uint32_t completion_expected;  // 有一次安装还没拿到"完成 + ping"两个证明
    std::uint32_t dangling;             // 有未证明安全的悬挂
    std::uint32_t done_signaled;        // 完成事件当前是否置位
};

// `DeskFeedProbeObserverStage` 的输出。
struct HookProbeStage {
    std::uint32_t size;      // = kProbeStageSize
    std::uint32_t version;   // = kProbeStageVersion
    std::uint32_t stage;     // HookObserverStage
    std::uint32_t sequence;  // 每次回调自增；用它可以判断"看到的阶段属于哪一次回调"
    std::uint32_t complete;  // 这一次回调是否已经走到 CallNextReturned 之后
    std::uint32_t reserved[2];
};
#pragma pack(pop)

static_assert(sizeof(HookProbeStatus) == kProbeStatusSize, "HookProbeStatus 必须是固定大小 POD");
static_assert(sizeof(HookProbeStage) == kProbeStageSize, "HookProbeStage 必须是固定大小 POD");

}  // namespace deskfeed::payload

// 导出的探针入口（实现见 src/payload/dllmain.cpp 的 DeskFeedProbe* 一组）在**这里故意不声明**：
// 探针侧一律按名字 `GetProcAddress`（不链接负载），而负载自己的定义带 `__declspec(dllexport)`，
// 与"没有 dllexport 的声明"放在一起会被 MSVC 判成不同链接（C2375）。导出名的权威清单在
// dllmain.cpp 里，随 ABI 一起维护：
//   DeskFeedProbeSetInstallDelay(unsigned)                     -> void   R1
//   DeskFeedProbeHookStatus(HookProbeStatus*, unsigned)        -> int    R2
//   DeskFeedProbeSetObserver(int)                              -> int    R3
//   DeskFeedProbeObserverStage(HookProbeStage*, unsigned)      -> int    R3
//   DeskFeedProbeSetCompletionMutation(unsigned)               -> int    R4
//   DeskFeedProbeSetTrayWindow(HWND)                           -> int    R5
// 全部是 `extern "C"` + `__stdcall`。
