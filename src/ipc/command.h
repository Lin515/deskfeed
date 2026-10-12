#pragma once

#include <string>
#include <vector>

// 命令行解析。冻结的四种形式（见 docs/lifecycle.md）：
//   deskfeed.exe --url "<URL>" --pid <PID>
//   deskfeed.exe --url "<URL>"
//   deskfeed.exe --quit-if-bound <PID>
//   deskfeed.exe
// 另外这些是实施层的附加开关，不改变上面四种形式的行为：
//   --quit                    无条件正常退出运行中的实例
//   --inspect / --dump-state  查询运行中的实例并打印一行 JSON
//   --scope / --test-instance 隔离命名空间（测试用，见 docs/testing.md）
//   --headless / --no-inject / --no-taskbar / --hidden / --mock  本次运行不碰界面与任务栏
//   --state-dir <DIR>         覆盖设置文件所在目录（仅本次运行）
//   --frame-mailbox           按 scope 建隔离的显示邮箱（只给离线验收用，必须与 --scope 和
//                             --headless / --no-inject 同时出现）
//   --poll-ms / --retry-ms / --timeout-ms  随 URL 一起提交调度配置
namespace deskfeed {

enum class CommandKind {
    Start,        // 无参数：已有实例则不接管；没有实例则用保存的 URL 独立启动
    Apply,        // --url [--pid]：设置/切换 URL，可选绑定生产者
    QuitIfBound,  // --quit-if-bound <PID>：只有绑定 PID 匹配时才退出
    Quit,         // --quit：无条件退出（实施层附加）
    Inspect,      // --inspect：查询运行中的实例
    Help,
};

struct Command {
    CommandKind kind = CommandKind::Start;

    // has_url 与 url 分开：显式的 --url "" 必须被拒绝，不能被当成“没有参数”
    // 而去沿用磁盘上保存的地址。
    bool has_url = false;
    std::wstring url;   // Apply
    bool has_pid = false;
    unsigned long pid = 0;  // Apply / QuitIfBound

    bool has_poll_ms = false;
    long long poll_ms = 0;
    bool has_retry_ms = false;
    long long retry_ms = 0;
    bool has_timeout_ms = false;
    long long timeout_ms = 0;

    // `--wait-exit-ms <1..60000>`：只对 `--quit` / `--quit-if-bound` 有效。缺省 0 = 原语义
    // （只等 IPC 确认，不等实例真的退出；确认不等于实例已经退出）。非 0 时发送端在拿到
    // `Accepted` 之后等到目标实例进程**真的退出**为止；到点还没退按“未确认退出”返回，
    // 绝不补杀、绝不重开、绝不制造第二个常驻实例。
    unsigned long wait_exit_ms = 0;

    // 隔离与本次运行的开关。
    std::wstring scope;      // 空 = 生产实例
    std::wstring state_dir;  // 空 = 默认位置
    bool headless = false;   // 不显示悬浮窗，也不碰任务栏
    bool no_taskbar = false; // 不碰任务栏（不注入 explorer）
    bool hidden = false;     // 悬浮窗保持隐藏
    // 显式要求建立隔离的显示邮箱。这不是“碰界面”：它只建一块按 scope 命名的共享内存与
    // 一个通知事件，用来在无界面、不注入的前提下离线验收“主进程 → 显示面”这条 seam。
    // 必须与 --scope 同时出现，否则会落到生产命名空间里。
    bool frame_mailbox = false;
};

bool command_parse(std::vector<std::wstring> const& args, Command& out, std::wstring& error);

// 帮助文本（UTF-8，用于标准错误/标准输出）。
std::string command_usage();

}  // namespace deskfeed
