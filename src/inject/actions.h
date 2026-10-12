#pragma once

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

int do_load(Options const& options);
int do_stop(Options const& options);
int do_status(Options const& options);
int do_watch(Options const& options);

// watch 的两轮之间要记住的东西。
struct WatchState {
    unsigned long pid = 0;
    bool was_active = false;
    std::wstring problem;
    unsigned long long next_attempt_ms = 0;
};

// watch 的一轮：返回 0 表示这一轮结束时目标进程里的负载处于"可证明在工作"的状态。
int watch_round(Options const& options, WatchState& state);

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
