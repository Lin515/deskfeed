// deskfeed.exe 的入口。命令行解析与全部逻辑在 master/main 的 master_main 里。

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "master/master.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    std::vector<std::wstring> args;
    int argc = 0;
    if (LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc)) {
        for (int index = 1; index < argc; ++index) {
            args.emplace_back(argv[index]);
        }
        ::LocalFree(argv);
    }
    return deskfeed::master_main(args);
}
