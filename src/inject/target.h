#pragma once

#include <string>

#include "inject/remote.h"

// 注入目标与实例 scope 的解析。
//
// 目标**只有**一个：当前任务栏属主，即 `Shell_TrayWnd`（次选 `Shell_SecondaryTrayWnd`）的属主
// 进程。刻意不做"进程列表里第一个 explorer.exe"这条退路：多个 explorer.exe 同时跑是常态
// （shell 崩溃/重启后旧进程赖着不走，用户手开的文件管理器也可能叫 explorer.exe），而只有持有
// 任务栏的那棵树里有托盘元素，装错进程会让读数块永远不出现。
namespace deskfeed::inject {

// 任务栏属主 PID；两个窗口都没有（explorer 正在重启的那一瞬间）返回 0。
unsigned long taskbar_owner_pid();

// 解析出属主进程；找不到就拒绝（返回 false，error 写明原因），不做任何回退猜测。
bool resolve_taskbar_owner(ProcessEntry& out, std::wstring& error);

// `--scope` 的值：空串表示生产实例（合法）；非空必须过实例名白名单（1..32 个
// [A-Za-z0-9_-]），否则拒绝整条命令——scope 会被带进内核对象名与文件路径。
bool resolve_scope(std::wstring const& text, std::wstring& scope, std::wstring& error);

}  // namespace deskfeed::inject
