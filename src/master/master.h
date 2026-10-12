#pragma once

#include <string>
#include <vector>

// 主进程（deskfeed.exe）的命令行入口。
//
// 单人份工作都在这里做完：仲裁单实例、转发命令、提交配置事务、取数、健康/到期状态机
// 与消息循环。返回的值就是进程退出码（见 ipc/command.h 的说明）。
namespace deskfeed {

int master_main(std::vector<std::wstring> const& args);

}  // namespace deskfeed
