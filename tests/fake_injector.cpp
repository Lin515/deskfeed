// 假注入器：任务栏编排（src/master/taskbar_supervisor.cpp）的**离线量具**，也是它永久用例的
// 被调对象（tests/master_supervisor_tests.cpp）。
//
// 它**不碰任何别的进程**：不打开进程、不写内存、不注入、不联网，也不碰界面。它只做两件事：
//   1. 把自己收到的整条命令行原样追加成日志里的一行；
//   2. 按环境变量要求的延迟与退出码退出。
// 编排层要验的就是“什么时候发了哪个动作、按什么顺序、一次动作还在跑时下一步等不等它”，
// 这些都能从那份日志里逐行读出来——顺序就是编排层的决策顺序。
//
// 用法（编排层就是这么调的，命令行契约见 master/taskbar_supervisor.h）：
//   deskfeed-fake-injector.exe --status [--scope NAME]
//   deskfeed-fake-injector.exe --load   [--payload DLL] [--scope NAME]
//   deskfeed-fake-injector.exe --stop   [--payload DLL] [--scope NAME]
//
// 环境变量：
//   DESKFEED_FAKE_LOG       日志文件路径（必填；没给就什么都不做并返回 9）
//   DESKFEED_FAKE_STATUS    --status 的退出码（缺省 1 = 未装载）
//   DESKFEED_FAKE_LOAD      --load 的退出码（缺省 0 = 成功）
//   DESKFEED_FAKE_STOP      --stop 的退出码（缺省 0 = 成功）
//   DESKFEED_FAKE_DELAY_MS  每次调用先睡这么久，用来制造“动作还在跑”的窗口（缺省 0）
//
// 退出码：9 = 这个桩自己没被正确配置（没有日志路径、没有动作或日志写不进去）、或者被要求的
// 退出码本身。用例必须把 9 当失败：它意味着“量具坏了”，而不是“编排层的行为符合预期”。
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::wstring environment(std::wstring const& name) {
    wchar_t buffer[512]{};
    DWORD const chars =
        ::GetEnvironmentVariableW(name.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (chars == 0 || chars >= std::size(buffer)) {
        return std::wstring();
    }
    return std::wstring(buffer, chars);
}

std::string utf8_of(std::wstring const& text) {
    if (text.empty()) {
        return std::string();
    }
    int const bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return std::string();
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes,
                          nullptr, nullptr);
    return out;
}

// 追加一行。每次调用单独开关文件：编排层同时只允许一个动作在跑，但“一个刚退出、下一个立刻
// 起来”是常态，所以不能靠缓存句柄，必须每次真的追加。
bool append_line(std::wstring const& path, std::string const& line) {
    HANDLE const file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::string const text = line + "\r\n";
    DWORD written = 0;
    BOOL const ok = ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                                nullptr);
    ::CloseHandle(file);
    return ok == TRUE && written == text.size();
}

int environment_exit_code(std::wstring const& name, int fallback) {
    std::wstring const value = environment(name);
    if (value.empty()) {
        return fallback;
    }
    return static_cast<int>(std::wcstol(value.c_str(), nullptr, 10));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring const log_path = environment(L"DESKFEED_FAKE_LOG");
    if (log_path.empty()) {
        return 9;
    }

    std::wstring action;
    for (int index = 1; index < argc; ++index) {
        std::wstring const arg(argv[index]);
        if (arg == L"--load" || arg == L"--stop" || arg == L"--status") {
            action = arg;
        }
    }
    if (action.empty()) {
        return 9;
    }

    std::string line;
    for (int index = 1; index < argc; ++index) {
        if (index > 1) {
            line += " ";
        }
        line += utf8_of(argv[index]);
    }
    if (!append_line(log_path, line)) {
        return 9;  // 量具坏了：静默继续会让用例“通过了但什么都没验”
    }

    std::wstring const delay = environment(L"DESKFEED_FAKE_DELAY_MS");
    if (!delay.empty()) {
        ::Sleep(static_cast<DWORD>(std::wcstoul(delay.c_str(), nullptr, 10)));
    }

    if (action == L"--status") {
        return environment_exit_code(L"DESKFEED_FAKE_STATUS", 1);
    }
    if (action == L"--load") {
        return environment_exit_code(L"DESKFEED_FAKE_LOAD", 0);
    }
    return environment_exit_code(L"DESKFEED_FAKE_STOP", 0);
}
