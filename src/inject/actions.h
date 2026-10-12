#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "inject/remote.h"

// 注入器的四个动作。命令行契约由主进程侧的任务栏编排层冻结（见 src/master/taskbar_supervisor.h）：
//
//   deskfeed-inject.exe --load   [--payload <DLL>] [--scope <NAME>] [--dry-run]
//   deskfeed-inject.exe --stop   [--payload <DLL>] [--scope <NAME>] [--no-unload]
//   deskfeed-inject.exe --status [--scope <NAME>]
//   deskfeed-inject.exe --watch  [--scope <NAME>] [--interval <秒>] [--once]
//
// 目标一律是当前任务栏属主；找不到属主就拒绝，不做"进程列表里第一个 explorer.exe"这种猜测。
// 退出码：0 = 成功/条件已满足；1 = 失败（load 没装上、stop 没能安全撤下）；2 = 用法或内部错误
// （含"--status 问不出来"）。--status 单独约定：0 = 负载在**这个** scope 上可证明地在工作；
// 1 = 没装上（或映射着但不工作，此时 --load 会把它换成新的）；2 = 问不出来/有悬挂无法安全退休。
//
// **每个动作都受 per-target 租约保护**（见 inject/guard.h 与 inject/leased_call.h）：
//   1. 解析目标 → `open_target_process` 捕获**一次**进程句柄与创建时间；
//   2. `GuardLease::acquire`：Acquired/Recovered 才继续，Busy 只报告（不排队、不强抢），
//      Unknown/Error 一律不做任何 Stop/Start/eject；
//   3. 动作期间每一次远程调用都按写序记账并在捕获的句柄上 park 到真实结论；
//   4. 只有"没有任何未决远程调用"才允许在唯一那个释放点 `ReleaseMutex`；否则保持事实并让本
//      actor 线程结束（`--watch` 的一轮也如此，绝不在同一条线程上进入下一轮再 acquire）。
namespace deskfeed::inject {

struct Options {
    std::wstring action;       // load / stop / status / watch
    std::wstring payload_path; // 空 = 与自身同目录的 kPayloadFileName
    std::wstring scope;        // 已过白名单；空 = 生产实例
    bool dry_run = false;      // --load：只报告会做什么
    bool unload = true;        // --stop：false = --no-unload，只让负载退出、保留映射
    bool once = false;         // --watch：只跑一轮
    unsigned interval_seconds = 5;  // --watch：两轮之间的秒数（1..60）
};

// 负载 DLL 的默认文件名（与注入器同目录）。
inline constexpr wchar_t kPayloadFileName[] = L"deskfeed-payload.dll";

// 负载导出的三个入口名（冻结 ABI）。
inline constexpr char const* kStartExportName = "DeskFeedStart";
inline constexpr char const* kStopExportName = "DeskFeedStop";

// 一次动作的目标环境。生产路径由 `resolve_action_target`（任务栏属主）填；用例填自己 spawn 的
// 隐藏夹具进程——**绝不**枚举或触碰真实 explorer。
struct ActionContext {
    unsigned long process_id = 0;
    std::uint64_t creation_time = 0;
    std::wstring image_path;
    // "别的实例"的镜像名：装载与卸载都要把别处残留的同一份负载按同一套安全门撤下。生产恒为
    // `explorer.exe`；用例传自己夹具主机的镜像名，于是清理逻辑只会在夹具进程之间发生。
    std::wstring other_image_name = L"explorer.exe";
};

// 生产路径的目标解析：只认任务栏属主（`Shell_TrayWnd`，次选 `Shell_SecondaryTrayWnd`）。
bool resolve_action_target(ActionContext& context, std::wstring& error);

int do_load(Options const& options);
int do_stop(Options const& options);
int do_status(Options const& options);
int do_watch(Options const& options);

// 目标无关的入口：把同一个动作跑在**显式给定**的目标上（用例驱动夹具进程时用；生产路径先
// `resolve_action_target` 再把结果传进来，两条路走的是同一份实现）。
int do_load_in(ActionContext const& context, Options const& options);
int do_stop_in(ActionContext const& context, Options const& options);
int do_status_in(ActionContext const& context, Options const& options);

// watch 的两轮之间要记住的东西。
struct WatchState {
    unsigned long pid = 0;
    bool was_active = false;
    std::wstring problem;
    unsigned long long next_attempt_ms = 0;
};

// watch 的一轮：返回 0 表示这一轮结束时目标进程里的负载处于"可证明在工作"的状态。
// 返回 `kWatchRoundLostUncertain` 表示这一轮以**未知**收尾：本 actor 线程必须结束（调用方不得在
// 同一条线程上进入下一轮再 acquire——Win32 mutex 是同线程递归的，那会把上一位留下的未知抹平）。
inline constexpr int kWatchRoundLostUncertain = 2;

int watch_round(Options const& options, WatchState& state);
int watch_round_in(ActionContext const& context, Options const& options, WatchState& state,
                   HANDLE interrupt_event = nullptr);

// 注入器自己的 DLL 路径（空 payload_path 时的默认值）。
std::wstring payload_path_for(Options const& options);

// 纯函数：给定 `DeskFeedStop` 的退出码、调用它的远程线程是否已退出、以及 --no-unload，
// 决定能不能 `FreeLibrary`。抽出来是为了它能被离线用例直接咬住（这条门禁是硬性的：
// 返回"未退休"时绝不卸载，也不按镜像名杀进程）。
enum class UnloadDecision {
    Unload,      // stop 报"可以卸载"且线程已退出
    KeepLoaded,  // 未退休：保留映射
    KeepNoUnload // --no-unload：本来就不卸
};

UnloadDecision unload_decision(DWORD stop_exit_code, bool thread_signaled, bool no_unload);

}  // namespace deskfeed::inject
