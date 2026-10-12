// 测试专用**夹具主机**：一个隐藏的、只做"装载夹具负载 DLL 然后等在那里"的进程。
//
// 它扮演"任务栏属主"这个角色，但只在用例自己 spawn 的进程里——**绝不**碰真实 explorer.exe，
// 也不起窗口。用例需要一个真实的远程目标：有真实的模块表、真实能建远程线程、真实可以被结束。
//
// 用法（由 deskfeed-tools-tests 自己拉起，不面向用户）：
//   deskfeed-tools-fixture-host.exe [--load-fixture <DLL>] [--parent <pid>] [--lifetime-ms <n>]
//
// 退出条件（两条，任一成立即退）：
//   * 父进程（用例进程）退出——绝不留孤儿；
//   * 到了 --lifetime-ms 的自毁兜底时刻——用例自己崩了也不留永久进程。
#include <windows.h>

#include <cstdlib>
#include <string>

int wmain(int argc, wchar_t** argv) {
    std::wstring fixture;
    unsigned long parent_pid = 0;
    unsigned long lifetime_ms = 60000;

    for (int index = 1; index < argc; ++index) {
        std::wstring const argument = argv[index];
        if (argument == L"--load-fixture" && index + 1 < argc) {
            fixture = argv[++index];
        } else if (argument == L"--parent" && index + 1 < argc) {
            parent_pid = ::wcstoul(argv[++index], nullptr, 10);
        } else if (argument == L"--lifetime-ms" && index + 1 < argc) {
            lifetime_ms = ::wcstoul(argv[++index], nullptr, 10);
        }
    }

    if (!fixture.empty()) {
        if (::LoadLibraryW(fixture.c_str()) == nullptr) {
            return 3;
        }
    }

    if (parent_pid != 0) {
        HANDLE const parent = ::OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
        if (parent != nullptr) {
            ::WaitForSingleObject(parent, lifetime_ms);
            ::CloseHandle(parent);
            return 0;
        }
    }
    ::Sleep(lifetime_ms);
    return 0;
}
