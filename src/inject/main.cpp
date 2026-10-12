// deskfeed-inject.exe — 把任务栏负载装进/撤出**当前任务栏属主** explorer.exe 的工具。
//
//   deskfeed-inject.exe --load   [--payload <DLL>] [--scope <NAME>] [--dry-run]
//   deskfeed-inject.exe --stop   [--payload <DLL>] [--scope <NAME>] [--no-unload]
//   deskfeed-inject.exe --status [--payload <DLL>] [--scope <NAME>]
//   deskfeed-inject.exe --watch  [--scope <NAME>] [--interval <秒>] [--once] [--dry-run]
//
// 目标恒为当前任务栏属主（Shell_TrayWnd，次选 Shell_SecondaryTrayWnd）；找不到就拒绝，不做
// "进程列表里第一个 explorer.exe"这种猜测。scope 为空 = 生产实例。
//
// 退出码：0 = 成功/条件已满足；1 = 失败；2 = 用法或内部错误。`--status` 的 0 还要求负载
// **可证明**在当前 scope 上工作（见 src/inject/README.md 的"状态"一节）。
//
// 本工具不动任何窗口、不结束任何进程、不按镜像名杀进程：它能做的只有"装载/卸载一个 DLL、
// 调它导出的三个入口、读它的一块定长状态缓冲"。
#include "inject/actions.h"
#include "inject/target.h"
#include "platform/log.h"
#include "platform/win32_util.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void print_usage() {
    std::wstring const text =
        L"usage: deskfeed-inject <--load|--stop|--status|--watch> [options]\n"
        L"  --load             load the payload into the taskbar owner and start it\n"
        L"                     (removes a stale payload first; never forces an unload)\n"
        L"  --stop             stop the payload and unload the DLL (the safe path only)\n"
        L"  --status           report whether the payload is provably active on this scope\n"
        L"  --watch            keep the payload loaded; re-injects after an owner restart\n"
        L"  --payload <DLL>    payload DLL (default: deskfeed-payload.dll next to this tool)\n"
        L"  --scope <NAME>     instance scope (1..32 of [A-Za-z0-9_-]); empty = production\n"
        L"  --dry-run          report what would happen; inject nothing, change nothing\n"
        L"  --no-unload        stop: let the payload exit without unmapping the DLL\n"
        L"  --interval <n>     watch: seconds between rounds (1..60, default 5)\n"
        L"  --once             watch: run one round and exit\n"
        L"\n"
        L"exit codes: 0 ok / 1 failed / 2 usage or internal error.\n"
        L"--status: 0 = mapped and provably active on this scope; 1 = not loaded (or mapped but\n"
        L"          not working; --load replaces it); 2 = cannot tell, or a dangling payload that\n"
        L"          must not be unloaded.\n";
    ::fputws(text.c_str(), stderr);
}

bool parse_arguments(int argc, wchar_t** argv, deskfeed::inject::Options& options) {
    if (argc < 2) {
        return false;
    }

    for (int index = 1; index < argc; ++index) {
        std::wstring const argument = argv[index];
        if (argument == L"--load") {
            if (!options.action.empty()) {
                return false;
            }
            options.action = L"load";
        } else if (argument == L"--stop") {
            if (!options.action.empty()) {
                return false;
            }
            options.action = L"stop";
        } else if (argument == L"--status") {
            if (!options.action.empty()) {
                return false;
            }
            options.action = L"status";
        } else if (argument == L"--watch") {
            if (!options.action.empty()) {
                return false;
            }
            options.action = L"watch";
        } else if (argument == L"--payload" && index + 1 < argc) {
            options.payload_path = argv[++index];
        } else if (argument == L"--scope" && index + 1 < argc) {
            std::wstring scope;
            std::wstring error;
            if (!deskfeed::inject::resolve_scope(argv[++index], scope, error)) {
                ::fwprintf(stderr, L"invalid --scope: %s\n", error.c_str());
                return false;
            }
            options.scope = scope;
        } else if (argument == L"--dry-run") {
            options.dry_run = true;
        } else if (argument == L"--no-unload") {
            options.unload = false;
        } else if (argument == L"--interval" && index + 1 < argc) {
            std::wstring const value = argv[++index];
            wchar_t* end = nullptr;
            unsigned long const parsed = ::wcstoul(value.c_str(), &end, 10);
            if (end == value.c_str() || parsed < 1 || parsed > 60) {
                ::fwprintf(stderr, L"invalid --interval value: %s (allowed 1..60 seconds)\n",
                           value.c_str());
                return false;
            }
            options.interval_seconds = static_cast<unsigned>(parsed);
        } else if (argument == L"--once") {
            options.once = true;
        } else {
            ::fwprintf(stderr, L"unknown argument: %s\n", argument.c_str());
            return false;
        }
    }

    return !options.action.empty();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    deskfeed::inject::Options options;
    if (!parse_arguments(argc, argv, options)) {
        print_usage();
        return 2;
    }

    // 日志每次都截断（同名会覆盖）：这个工具是短命的，历史由主进程的日志与它自己的输出去留。
    bool const logged =
        deskfeed::log_open(deskfeed::default_log_path(L"deskfeed-inject.log"), true);
    deskfeed::log_line(L"deskfeed inject");
    if (!logged) {
        ::fwprintf(stderr, L"warning: cannot write the log file, console output only\n");
    }

    if (options.action == L"load") {
        return deskfeed::inject::do_load(options);
    }
    if (options.action == L"stop") {
        return deskfeed::inject::do_stop(options);
    }
    if (options.action == L"status") {
        return deskfeed::inject::do_status(options);
    }
    if (options.action == L"watch") {
        return deskfeed::inject::do_watch(options);
    }

    print_usage();
    return 2;
}
