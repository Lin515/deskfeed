#include "ipc/command.h"

#include <cstdlib>

namespace deskfeed {
namespace {

bool is_option(std::wstring const& text) { return text.rfind(L"--", 0) == 0; }

bool take_value(std::vector<std::wstring> const& args, std::size_t& index, std::wstring& value) {
    if (index + 1 >= args.size() || is_option(args[index + 1])) {
        return false;
    }
    value = args[++index];
    return true;
}

bool parse_u32(std::wstring const& text, unsigned long& out) {
    if (text.empty()) {
        return false;
    }
    for (wchar_t const ch : text) {
        if (ch < L'0' || ch > L'9') {
            return false;
        }
    }
    wchar_t* end = nullptr;
    unsigned long long const value = std::wcstoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != L'\0' || value > 0xFFFFFFFFULL) {
        return false;
    }
    out = static_cast<unsigned long>(value);
    return true;
}

bool parse_i64(std::wstring const& text, long long& out) {
    if (text.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    long long const value = std::wcstoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != L'\0') {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

bool command_parse(std::vector<std::wstring> const& args, Command& out, std::wstring& error) {
    Command command;
    int primary = 0;  // 已经出现过几种“主命令”
    for (std::size_t index = 0; index < args.size(); ++index) {
        std::wstring const& arg = args[index];
        if (arg == L"-h" || arg == L"--help" || arg == L"/?") {
            out = Command{};
            out.kind = CommandKind::Help;
            return true;
        }
        if (arg == L"--url") {
            std::wstring value;
            if (!take_value(args, index, value)) {
                error = L"--url 后面要跟快照地址";
                return false;
            }
            command.url = value;
            command.has_url = true;
            ++primary;
        } else if (arg == L"--pid") {
            std::wstring value;
            if (!take_value(args, index, value)) {
                error = L"--pid 后面要跟进程号";
                return false;
            }
            if (!parse_u32(value, command.pid)) {
                error = L"--pid 不是合法的进程号：" + value;
                return false;
            }
            command.has_pid = true;
        } else if (arg == L"--quit-if-bound") {
            std::wstring value;
            if (!take_value(args, index, value)) {
                error = L"--quit-if-bound 后面要跟进程号";
                return false;
            }
            if (!parse_u32(value, command.pid)) {
                error = L"--quit-if-bound 不是合法的进程号：" + value;
                return false;
            }
            command.kind = CommandKind::QuitIfBound;
            ++primary;
        } else if (arg == L"--quit") {
            command.kind = CommandKind::Quit;
            ++primary;
        } else if (arg == L"--inspect" || arg == L"--dump-state") {
            command.kind = CommandKind::Inspect;
            ++primary;
        } else if (arg == L"--poll-ms") {
            std::wstring value;
            if (!take_value(args, index, value) || !parse_i64(value, command.poll_ms)) {
                error = L"--poll-ms 后面要跟毫秒数";
                return false;
            }
            command.has_poll_ms = true;
        } else if (arg == L"--retry-ms") {
            std::wstring value;
            if (!take_value(args, index, value) || !parse_i64(value, command.retry_ms)) {
                error = L"--retry-ms 后面要跟毫秒数";
                return false;
            }
            command.has_retry_ms = true;
        } else if (arg == L"--timeout-ms") {
            std::wstring value;
            if (!take_value(args, index, value) || !parse_i64(value, command.timeout_ms)) {
                error = L"--timeout-ms 后面要跟毫秒数";
                return false;
            }
            command.has_timeout_ms = true;
        } else if (arg == L"--scope" || arg == L"--test-instance") {
            if (!take_value(args, index, command.scope)) {
                error = L"--scope 后面要跟实例名";
                return false;
            }
            if (command.scope.empty()) {
                // 显式的空实例名不能被当成“生产实例”，否则一次写错就落到真实命名空间里。
                error = L"--scope 的实例名不能是空字符串";
                return false;
            }
        } else if (arg == L"--state-dir") {
            if (!take_value(args, index, command.state_dir)) {
                error = L"--state-dir 后面要跟目录";
                return false;
            }
        } else if (arg == L"--headless" || arg == L"--mock") {
            // --mock 是 --headless 的别名：只关界面与注入，不伪造任何数据。
            command.headless = true;
        } else if (arg == L"--no-inject" || arg == L"--no-taskbar") {
            command.no_taskbar = true;
        } else if (arg == L"--hidden") {
            command.hidden = true;
        } else if (arg == L"--frame-mailbox") {
            command.frame_mailbox = true;
        } else if (arg == L"--wait-exit-ms") {
            std::wstring value;
            if (!take_value(args, index, value)) {
                error = L"--wait-exit-ms 后面要跟毫秒数";
                return false;
            }
            long long parsed = 0;
            if (!parse_i64(value, parsed) || parsed < 1 || parsed > 60000) {
                error = L"--wait-exit-ms 只接受 1..60000 的毫秒数：" + value;
                return false;
            }
            command.wait_exit_ms = static_cast<unsigned long>(parsed);
        } else {
            error = L"不认识的参数：" + arg;
            return false;
        }
    }

    if (primary > 1) {
        error = L"--url / --quit-if-bound / --quit / --inspect 不能同时使用";
        return false;
    }
    if (command.has_url && command.url.empty()) {
        // 显式的空地址必须当场拒绝：它既不是“没给地址”，也不能去覆盖磁盘上的旧地址。
        error = L"--url 的地址不能是空字符串";
        return false;
    }
    if (command.has_pid && !command.has_url) {
        error = L"--pid 必须与 --url 一起使用";
        return false;
    }
    if (command.wait_exit_ms != 0 && command.kind != CommandKind::Quit &&
        command.kind != CommandKind::QuitIfBound) {
        // 只有退出类命令才有“等实例真的退出”这个语义；接管/查询等命令带上它一定是写错了。
        error = L"--wait-exit-ms 只能与 --quit 或 --quit-if-bound 一起使用";
        return false;
    }
    if (command.headless) {
        command.no_taskbar = true;
        command.hidden = true;
    }
    if (command.frame_mailbox) {
        // 这条 seam 只给**按 scope 隔离的无界面**运行用：它建的是隔离命名空间里的共享内存与
        // 通知事件，不碰界面也不注入，但绝不能在没给 scope 时打开——那会落到生产命名空间。
        if (command.scope.empty()) {
            error = L"--frame-mailbox 必须与 --scope 一起使用（它只用于隔离实例的离线验收）";
            return false;
        }
        if (!command.no_taskbar) {
            error = L"--frame-mailbox 必须与 --headless 或 --no-inject 一起使用";
            return false;
        }
    }
    if (command.kind == CommandKind::Start && command.has_url) {
        command.kind = CommandKind::Apply;
    }
    out = command;
    return true;
}

std::string command_usage() {
    return
        "deskfeed.exe - Windows 桌面信息显示（DeskFeed）\n"
        "\n"
        "用法:\n"
        "  deskfeed.exe --url \"<URL>\" --pid <PID>   设置/切换地址并绑定生产者进程\n"
        "  deskfeed.exe --url \"<URL>\"               设置/切换地址，并解除原来的绑定\n"
        "  deskfeed.exe --quit-if-bound <PID>        仅当前绑定 PID 匹配时正常退出\n"
        "  deskfeed.exe                              用最后保存的地址独立启动\n"
        "\n"
        "附加开关:\n"
        "  --quit                    无条件正常退出运行中的实例\n"
        "  --inspect, --dump-state   查询运行中的实例并打印一行 JSON\n"
        "  --poll-ms / --retry-ms / --timeout-ms <毫秒>   随 --url 一起提交调度配置\n"
        "  --scope <名字>            使用隔离的实例命名空间（测试用；别名 --test-instance）\n"
        "                            环境变量 DESKFEED_SCOPE / DESKFEED_TEST_INSTANCE 同义，\n"
        "                            命令行优先；实例名只允许字母、数字、连字符与下划线，最长 32\n"
        "  --state-dir <目录>        本次运行改用指定目录里的 settings.ini\n"
        "  --headless, --mock        本次运行不显示悬浮窗、不碰任务栏（--mock 不伪造数据）\n"
        "  --no-inject, --no-taskbar 本次运行不碰任务栏\n"
        "  --hidden                  本次运行保持悬浮窗隐藏\n"
        "  --frame-mailbox           按 scope 建隔离的显示邮箱（只给离线验收用；必须与\n"
        "                            --scope 以及 --headless / --no-inject 同时出现）\n"
        "  --wait-exit-ms <1..60000> 只与 --quit / --quit-if-bound 同用：确认被接受后等到实例\n"
        "                            真的退出；缺省 0 = 只等确认（确认不等于实例已退出）\n"
        "  -h, --help                这一段\n"
        "\n"
        "退出码: 0 命令被接受（或条件退出无需操作）；2 用法错误；3 PID 不合法；\n"
        "        4 配置保存失败；5 IPC 超时或没有实例；6 命令被拒绝；7 内部错误。\n";
}

}  // namespace deskfeed
