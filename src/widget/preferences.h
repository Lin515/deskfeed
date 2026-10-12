#pragma once

#include <string>

#include "ipc/settings.h"
#include "widget/window.h"  // WindowPreferences

// settings.ini 的 `[window]` 一节 ↔ WindowPreferences。
//
// 键名与含义见 docs/configuration.md：`right` / `bottom`（右下角锚点）、`expanded`、`topmost`、
// `hidden`。这里只碰 `[window]` 一节：
//   * `[source]` 的地址与调度归主进程的命令事务（换源与界面切换各写各的键，互不覆盖）；
//   * `[taskbar] component` 归主进程的任务栏开关动作（在那个动作里落盘一次，不重复写）。
//
// 两个函数都**只借用**主进程那一份 SettingsFile：不持有、不重新 load。主进程是唯一改这个
// 文件的人，界面切换因此不可能把刚换过的地址覆盖回旧值。窗口自己不落盘任何“只在本次运行
// 有效”的开关（`--hidden` / `--no-inject` / `--scope` 之类一律不写）。
namespace deskfeed::widget {

// 读 `[window]`。缺省：没有位置键 → 锚点未知（按默认右下角落位）；expanded=false；
// topmost=true；hidden=false（缺省显示）。数值键读不出来时按缺省处理，不写坏文件。
WindowPreferences read_window_preferences(SettingsFile const& settings);

// 写 `[window]` 并原子保存（先写同目录临时文件再替换，见 ipc/settings.h）。
// 锚点未知时不写 `right`/`bottom`。失败时 error 说明原因，磁盘保持原样；返回 false 时
// 调用方（窗口）必须把内存里的偏好回滚到落盘前的那一份。
//
// **它会改传进来的那个 SettingsFile 对象**：`set` 改内存里的行，保存失败时再 `load` 一次把
// 脏改动丢回磁盘状态。所以主进程要用 “clone → 改 → 保存 → 成功才 adopt” 的顺序：在一份
// **候选副本**上调用它（`SettingsFile candidate = *settings_;`，这个类可拷贝），只有返回 true
// 才把候选换成当前使用的实例。直接对正在用的那个实例调用，一旦保存失败、连重新 load 也失败，
// 内存里的 URL / 调度就可能与磁盘不一致。
bool write_window_preferences(SettingsFile& settings, WindowPreferences const& preferences,
                              std::wstring* error);

}  // namespace deskfeed::widget
