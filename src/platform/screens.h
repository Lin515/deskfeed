#pragma once

#include <windows.h>

#include <string>
#include <vector>

// 显示器与任务栏窗口的枚举。主进程与负载共用这一份：负载据此把每一块任务栏落到哪个屏幕，
// 主进程据此生成右键菜单里的屏幕项。两边对“屏幕标识”和“屏幕标签”的解释因此只有一处。
namespace deskfeed {

struct ScreenInfo {
    // 稳定标识：显示器接口路径（`\\?\DISPLAY#...`）。枚举不到时退化成 `\\.\DISPLAYn`。
    std::wstring id;
    // 当前拓扑里的显示设备名（`\\.\DISPLAY1`），只用于诊断日志，不写配置。
    std::wstring device_name;
    // 面向用户的标签：`屏幕 1（主）` / `屏幕 2`，按屏幕坐标排序后编号。
    std::wstring label;
    RECT bounds{};
    bool primary = false;
};

// 当前所有活动显示器，按左上角坐标排序。失败时返回空表（调用方按“没有屏幕”处理，不猜测）。
std::vector<ScreenInfo> enumerate_screens();

// 当前主屏的稳定标识；拿不到时为空。
std::wstring primary_screen_id();

// 窗口所在屏幕的稳定标识；窗口无效或不在任何显示器上时为空。
std::wstring screen_id_for_window(HWND window);

struct TaskbarWindowInfo {
    HWND window = nullptr;
    DWORD thread_id = 0;
    std::wstring screen_id;
    std::wstring label;
    bool primary = false;
};

// 所有可见任务栏窗口（主任务栏与每块副屏的 `Shell_SecondaryTrayWnd`），按屏幕顺序。
// 没有任务栏窗口的屏幕不会出现在这里——那正是运行期“这块屏幕不可用于托盘显示”的判据。
std::vector<TaskbarWindowInfo> enumerate_taskbar_windows();

}  // namespace deskfeed
