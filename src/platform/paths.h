#pragma once

#include <string>

// 用户级路径与实例命名。
//
// 生产实例：
//   设置   %APPDATA%\DeskFeed\settings.ini
//   日志   %LOCALAPPDATA%\DeskFeed\deskfeed.log
// 测试实例（--test-instance <后缀> 或环境变量 DESKFEED_TEST_INSTANCE）：
//   设置   %APPDATA%\DeskFeed\test-<后缀>\settings.ini
//   日志   %LOCALAPPDATA%\DeskFeed\test-<后缀>\deskfeed.log
// 测试实例自成一套命名（互斥体、控制窗口类名、设置、日志），既不抢生产实例的单例，
// 也不会被生产实例挡住，见 docs/testing.md。
namespace deskfeed {

std::wstring instance_suffix();  // 环境变量里给的后缀；生产实例为空串

// 实例名的白名单校验：只允许 [A-Za-z0-9_-]，长度 1..32。空串表示生产实例，合法。
// 拒绝其它字符是为了不把实例名带进文件路径或内核对象名（避免路径/对象名穿越）。
bool scope_validate(std::wstring const& scope, std::wstring& error);

// 当前用户的 SID 文本（"S-1-5-21-..."）；取不到时返回空串。
std::wstring current_user_sid();

// 把基础名字扩成“当前用户 + 实例”范围的名字，例如：
//   base=L"Local\\DeskFeed-master", suffix=L""        -> Local\DeskFeed-master-1a2b3c4d
//   base=L"Local\\DeskFeed-master", suffix=L"acc1"    -> Local\DeskFeed-master-test-acc1-1a2b3c4d
// 末尾的短哈希来自当前用户 SID，避免同一登录会话里不同用户撞名。
std::wstring scoped_name(std::wstring const& base, std::wstring const& suffix);

std::wstring deskfeed_settings_path(std::wstring const& suffix);
std::wstring deskfeed_log_path(std::wstring const& suffix);

// %LOCALAPPDATA%\DeskFeed[\test-<suffix>]：日志与 --inspect 的转交文件都放这里。
std::wstring deskfeed_runtime_dir(std::wstring const& suffix);

// --inspect 的转交路径。主进程按请求里的发送方 PID 写出这个文件，客户端读完即删；
// 主进程只允许写这个目录下的文件，避免被当作任意写文件的工具。
std::wstring deskfeed_inspect_path(std::wstring const& suffix, unsigned long sender_pid);

// 确保父目录存在（创建全部中间层）。失败返回 false，error 里是原因。
bool ensure_parent_directory(std::wstring const& file_path, std::wstring& error);

// 简单的整文件读写（--inspect 的转交文件、检查器的输入都用它）。
bool write_file_bytes(std::wstring const& path, std::string const& bytes, std::wstring& error);
bool read_file_bytes(std::wstring const& path, std::string& bytes, bool& exists, std::wstring& error);
bool delete_file(std::wstring const& path);

}  // namespace deskfeed
