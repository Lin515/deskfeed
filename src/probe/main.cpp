// deskfeed-probe.exe —— 任务栏负载的**自测宿主**：在一个自己拥有的 XAML 岛里跑与注入路径完全
// 相同的代码，不必把负载装进 explorer.exe。
//
//   deskfeed-probe.exe --mock [--cores 1|2|3] [--feed] [--dll <payload.dll>] [--scope <name>]
//                      [--seconds <n>] [--dump-only] [--hook] [--teardown] [--balance-check]
//                      [--third-party-hook-pump] [--install-race [--install-delay-ms <n>]]
//                      [--mutation-early-done] [--hang-ui-ms <n>] [--slow-apply-ms <n>]
//   deskfeed-probe.exe --reading      <snapshot.json> [--refresh-ms <n>]   (严格快照 v1 量具)
//   deskfeed-probe.exe --reading-rows <snapshot.json> [--refresh-ms <n>]
//   deskfeed-probe.exe --frame-reading <frame.json>                       (仅诊断的显示帧量具)
//
// 三种离线量具**先于**任何窗口/线程/XAML/DLL：它们只把一份正文换算成读数文本，输出要与
// 桌面悬浮窗逐字节可比。`--mock` 与 `--feed` 互斥：前者在宿主线程上 inline 解析并应用（结构
// 自测），后者把宿主的岛登记成负载的托盘目标、跑**生产入口**（负载经真实 hook 编组到 UI 线程）。
#include "probe/host.h"
#include "probe/offline.h"

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
        L"usage: deskfeed-probe <--reading|--reading-rows|--frame-reading <file>> [--refresh-ms <n>]\n"
        L"       deskfeed-probe --mock [--cores 1|2|3] [--feed] [--dll <payload.dll>]\n"
        L"                          [--scope <name>] [--seconds <n>] [--dump-only] [--hook]\n"
        L"                          [--teardown] [--balance-check] [--third-party-hook-pump]\n"
        L"                          [--install-race] [--install-delay-ms <n>]\n"
        L"                          [--mutation-early-done] [--hang-ui-ms <n>]\n"
        L"                          [--slow-apply-ms <n>]\n"
        L"\n"
        L"  --mock                  run the self-test host against the payload's probe exports\n"
        L"  --feed                  run the production entry point against the probe's own mailbox\n"
        L"                          (mutually exclusive with the inline --mock path)\n"
        L"  --cores <n>             how many XAML cores this process owns (default 1)\n"
        L"  --dll <path>            payload DLL (default: deskfeed-payload.dll next to this tool)\n"
        L"  --scope <name>          isolated instance scope (default probe)\n"
        L"  --third-party-hook-pump install a third-party WH_GETMESSAGE hook and check the\n"
        L"                          completion-gate ordering\n"
        L"  --mutation-early-done   adversarial failpoint: expect the ordering check to bite\n"
        L"  --install-race          delay publishing the hook handle (install/publish race)\n"
        L"  --reading/--reading-rows/--frame-reading   offline gauges (no window, no DLL)\n";
    ::fputws(text.c_str(), stderr);
}

struct ParseResult {
    bool ok = false;
    bool offline = false;
    deskfeed::probe::OfflineMode mode = deskfeed::probe::OfflineMode::Reading;
    std::wstring file;
    long long refresh_ms = 30000;
    deskfeed::probe::HostOptions host;
};

bool parse(int argc, wchar_t** argv, ParseResult& out) {
    bool have_offline = false;
    bool have_mock = false;
    for (int index = 1; index < argc; ++index) {
        std::wstring const argument = argv[index];
        auto next = [&](std::wstring& value) {
            if (index + 1 >= argc) {
                return false;
            }
            value = argv[++index];
            return true;
        };
        if (argument == L"--reading") {
            if (!next(out.file)) return false;
            out.mode = deskfeed::probe::OfflineMode::Reading;
            have_offline = true;
        } else if (argument == L"--reading-rows") {
            if (!next(out.file)) return false;
            out.mode = deskfeed::probe::OfflineMode::ReadingRows;
            have_offline = true;
        } else if (argument == L"--frame-reading") {
            if (!next(out.file)) return false;
            out.mode = deskfeed::probe::OfflineMode::FrameReading;
            have_offline = true;
        } else if (argument == L"--refresh-ms") {
            std::wstring value;
            if (!next(value)) return false;
            out.refresh_ms = static_cast<long long>(::wcstoll(value.c_str(), nullptr, 10));
        } else if (argument == L"--mock") {
            have_mock = true;
        } else if (argument == L"--feed") {
            out.host.feed = true;
        } else if (argument == L"--dll") {
            if (!next(out.host.dll_path)) return false;
        } else if (argument == L"--scope") {
            std::wstring value;
            if (!next(value)) return false;
            std::wstring scope;
            std::wstring error;
            if (!deskfeed::inject::resolve_scope(value, scope, error)) {
                ::fwprintf(stderr, L"invalid --scope: %s\n", error.c_str());
                return false;
            }
            out.host.scope = scope;
        } else if (argument == L"--cores") {
            std::wstring value;
            if (!next(value)) return false;
            unsigned long const cores = ::wcstoul(value.c_str(), nullptr, 10);
            if (cores < 1 || cores > 8) {
                return false;
            }
            out.host.cores = static_cast<unsigned>(cores);
        } else if (argument == L"--seconds") {
            std::wstring value;
            if (!next(value)) return false;
            out.host.seconds = static_cast<unsigned>(::wcstoul(value.c_str(), nullptr, 10));
        } else if (argument == L"--dump-only") {
            out.host.dump_only = true;
        } else if (argument == L"--hook") {
            out.host.hook = true;
        } else if (argument == L"--teardown") {
            out.host.teardown = true;
        } else if (argument == L"--balance-check") {
            out.host.balance_check = true;
        } else if (argument == L"--third-party-hook-pump") {
            out.host.third_party = true;
        } else if (argument == L"--mutation-early-done") {
            out.host.mutation_early_done = true;
            out.host.third_party = true;
        } else if (argument == L"--install-race") {
            out.host.install_delay_ms = 300;
        } else if (argument == L"--install-delay-ms") {
            std::wstring value;
            if (!next(value)) return false;
            out.host.install_delay_ms = static_cast<unsigned>(::wcstoul(value.c_str(), nullptr, 10));
        } else if (argument == L"--hang-ui-ms") {
            std::wstring value;
            if (!next(value)) return false;
            out.host.hang_ui_ms = static_cast<unsigned>(::wcstoul(value.c_str(), nullptr, 10));
        } else if (argument == L"--slow-apply-ms") {
            std::wstring value;
            if (!next(value)) return false;
            out.host.slow_apply_ms = static_cast<unsigned>(::wcstoul(value.c_str(), nullptr, 10));
        } else {
            ::fwprintf(stderr, L"unknown argument: %s\n", argument.c_str());
            return false;
        }
    }

    if (have_offline && (have_mock || out.host.feed)) {
        return false;  // 离线量具不碰窗口/线程/DLL
    }
    if (have_offline) {
        out.offline = true;
        out.ok = true;
        return true;
    }
    if (!have_mock && !out.host.feed) {
        return false;
    }
    out.ok = true;
    return true;
}

std::wstring directory_of(std::wstring const& path) {
    std::wstring::size_type const slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ParseResult parsed;
    if (!parse(argc, argv, parsed)) {
        print_usage();
        return 2;
    }

    if (parsed.offline) {
        // 离线这一半不建日志、不起窗口、不起线程、不加载 DLL，也不碰 explorer。
        int exit_code = 0;
        deskfeed::probe::run_offline(parsed.mode, parsed.file, parsed.refresh_ms, exit_code);
        return exit_code;
    }

    deskfeed::probe::HostOptions options = parsed.host;
    if (options.dll_path.empty()) {
        options.dll_path = deskfeed::join_path(directory_of(deskfeed::module_self_path()),
                                              L"deskfeed-payload.dll");
    }
    if (::GetFileAttributesW(options.dll_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ::fwprintf(stderr, L"payload DLL not found: %s\n", options.dll_path.c_str());
        return 2;
    }

    deskfeed::log_open(deskfeed::default_log_path(L"deskfeed-probe.log"), true);
    deskfeed::log_line(L"deskfeed probe host");
    if (options.install_delay_ms != 0 && options.install_delay_ms != 300) {
        deskfeed::log_format(L"install/publish delay overridden to %u ms", options.install_delay_ms);
    }

    int const code = deskfeed::probe::run_host(options);
    deskfeed::log_format(L"probe finished (%s)", code == 0 ? L"ok" : L"failed");
    deskfeed::log_close();
    return code;
}
