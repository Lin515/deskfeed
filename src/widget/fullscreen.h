#pragma once

#include <windows.h>

#include <string>

// 全屏判定：现在这一屏是不是被一个全屏程序占着。
//
// 悬浮窗只在“前台是全屏程序”时整窗藏起来，所以这里问的是前台窗口，不问我们自己的窗口。
// 判定刻意只用几个便宜的窗口查询，不枚举进程、不取数。
namespace deskfeed::widget {

// 命中哪条信号（要记进日志，方便真机验收对账是 A 还是 B 命中的）。
enum class FullscreenSignal {
    kNone = 0,
    // A：前台窗口完整盖住它所在显示器的 rcMonitor（无边框全屏、满屏窗口）。
    kMonitorCovered,
    // B：SHQueryUserNotificationState 报 QUNS_RUNNING_D3D_FULL_SCREEN（独占全屏，
    //    那种情况下窗口矩形根本量不准，只能信系统这个判据）。
    kD3DFullScreenExclusive,
};

// 现在的前台窗口算不算全屏。命中时把“哪个信号 + 前台窗口类名与矩形”写进 detail，
// 调用方直接拿它拼日志。
//
// 任何一步失败（拿不到前台窗口、读不到类名/矩形/显示器信息、系统状态查询报错）都返回
// kNone 并让 detail 尽量说清是哪一步失败：**判不出来一律不隐藏**，宁可显示，也不许把
// 用户本来就该看见的窗口藏没了。
FullscreenSignal fullscreen_foreground(std::wstring* detail);

// 信号名，只用于拼日志（"none" / "foreground-monitor-covered" / "d3d-fullscreen-exclusive"）。
wchar_t const* fullscreen_signal_name(FullscreenSignal signal);

}  // namespace deskfeed::widget
