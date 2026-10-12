// deskfeed-check：离线协议检查器与 HTTP 量具。
//
// See docs/testing.md. 关键约束：
//   * 不建互斥体、不开窗口、不注入任何进程；--url 用的就是主进程那份 http_get 与
//     snapshot_validate，所以“主进程会接纳什么”可以用一条命令问清楚；
//   * 退出码固定：0 接受、2 拒绝（HTTP 或校验）、1 用法/内部错误。

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/snapshot.h"
#include "core/utf8.h"
#include "net/http_client.h"
#include "platform/console.h"
#include "platform/paths.h"

namespace {

using deskfeed::ItemState;
using deskfeed::Snapshot;
using deskfeed::ValidationKind;
using deskfeed::ValidationResult;

struct Options {
    std::wstring file;
    std::wstring url;
    bool use_stdin = false;
    bool report_json = false;
    bool help = false;
    long long timeout_ms = 8000;
};

std::string const kUsage =
    "deskfeed-check: 快照协议检查器\n"
    "\n"
    "用法:\n"
    "  deskfeed-check --file <路径> [--report json]\n"
    "  deskfeed-check --stdin [--report json]\n"
    "  deskfeed-check --url <地址> [--timeout-ms <毫秒>] [--report json]\n"
    "  deskfeed-check --help\n"
    "\n"
    "退出码: 0 接受；2 拒绝（HTTP 或校验）；1 用法错误 / 内部错误。\n"
    "--report json 打印一行机器可读的结果；省略则打印人读的摘要。\n";

bool parse_options(std::vector<std::wstring> const& args, Options& out, std::string& error) {
    for (std::size_t index = 0; index < args.size(); ++index) {
        std::wstring const& arg = args[index];
        auto take = [&](std::wstring& value) {
            if (index + 1 >= args.size()) {
                return false;
            }
            value = args[++index];
            return true;
        };
        if (arg == L"-h" || arg == L"--help") {
            out.help = true;
        } else if (arg == L"--file") {
            if (!take(out.file)) {
                error = "--file 后面要跟路径";
                return false;
            }
        } else if (arg == L"--url") {
            if (!take(out.url)) {
                error = "--url 后面要跟地址";
                return false;
            }
        } else if (arg == L"--stdin") {
            out.use_stdin = true;
        } else if (arg == L"--report") {
            std::wstring value;
            if (!take(value) || value != L"json") {
                error = "--report 目前只支持 json";
                return false;
            }
            out.report_json = true;
        } else if (arg == L"--timeout-ms") {
            std::wstring value;
            if (!take(value)) {
                error = "--timeout-ms 后面要跟毫秒数";
                return false;
            }
            wchar_t* end = nullptr;
            out.timeout_ms = std::wcstoll(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != L'\0' || out.timeout_ms <= 0) {
                error = "--timeout-ms 不是合法的毫秒数";
                return false;
            }
        } else {
            error = "不认识的参数";
            return false;
        }
    }

    int const sources = (out.file.empty() ? 0 : 1) + (out.url.empty() ? 0 : 1) +
                        (out.use_stdin ? 1 : 0);
    if (!out.help && sources == 0) {
        error = "要给出 --file、--stdin 或 --url 之一";
        return false;
    }
    if (sources > 1) {
        error = "--file / --stdin / --url 只能用其中一个";
        return false;
    }
    return true;
}

// 按**字节流**读标准输入：不区分 stdin 是文件重定向还是管道。
//
// 管道那条路径有个坑：写端关闭之后 ReadFile 会以 ERROR_BROKEN_PIPE 失败，而那是正常的
// 流结束，不是读取错误。把它当成失败会让所有程序化调用者（Node / Python 起子进程、任何
// `... | deskfeed-check` 的用法）拿不到结果——只有 shell 的 `< file` 能用。
bool read_stdin(std::string& bytes, std::string& error) {
    HANDLE const handle = ::GetStdHandle(STD_INPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        error = "没有标准输入";
        return false;
    }
    char buffer[8192];
    for (;;) {
        DWORD read = 0;
        if (::ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) == FALSE) {
            DWORD const code = ::GetLastError();
            if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) {
                break;  // 管道写端/文件末尾：正常的流结束
            }
            error = "读标准输入失败";
            return false;
        }
        if (read == 0) {
            break;
        }
        bytes.append(buffer, read);
        if (bytes.size() > 2 * 1024 * 1024) {
            error = "标准输入超过 2 MiB";
            return false;
        }
    }
    return true;
}

std::string json_escape(std::string const& text) {
    std::string out;
    for (unsigned char const byte : text) {
        switch (byte) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    char buffer[8]{};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(byte));
                }
        }
    }
    return out;
}

std::string quote(std::string const& text) { return "\"" + json_escape(text) + "\""; }

std::string report_json(bool accept, char const* kind, std::string const& message, std::size_t bytes,
                        Snapshot const* snapshot) {
    std::string out = "{";
    out += "\"accept\":" + std::string(accept ? "true" : "false");
    out += ",\"kind\":" + quote(kind);
    out += ",\"message\":" + quote(message);
    out += ",\"bodyBytes\":" + std::to_string(bytes);
    if (snapshot != nullptr && accept) {
        out += ",\"version\":1";
        if (snapshot->has_expires_at) {
            out += ",\"expiresAtMs\":" + std::to_string(snapshot->expires_at_ms);
        } else {
            out += ",\"expiresAtMs\":null";
        }
        out += ",\"items\":[";
        for (std::size_t index = 0; index < snapshot->items.size(); ++index) {
            auto const& item = snapshot->items[index];
            if (index > 0) {
                out += ",";
            }
            out += "{\"id\":" + quote(item.id) + ",\"label\":" + quote(item.label) +
                   ",\"value\":" + quote(item.value) +
                   ",\"state\":" + quote(deskfeed::item_state_name(item.state)) +
                   ",\"detail\":" + quote(item.detail) + "}";
        }
        out += "]";
    } else {
        out += ",\"version\":null,\"expiresAtMs\":null,\"items\":[]";
    }
    out += "}";
    return out;
}

void print_human(bool accept, char const* kind, std::string const& message, std::size_t bytes,
                 Snapshot const* snapshot) {
    if (accept && snapshot != nullptr) {
        std::string text = "accepted: " + std::to_string(snapshot->items.size()) + " item(s), " +
                           std::to_string(bytes) + " byte(s)";
        if (snapshot->has_expires_at) {
            text += ", expiresAtMs=" + std::to_string(snapshot->expires_at_ms);
        }
        deskfeed::console_write_stdout(text + "\n");
        for (auto const& item : snapshot->items) {
            deskfeed::console_write_stdout("  " + item.id + " | " + item.label + " | " +
                                          item.value + " | " + deskfeed::item_state_name(item.state) +
                                          "\n");
        }
        return;
    }
    deskfeed::console_write_stdout("rejected: " + std::string(kind) + ": " + message + "\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> args;
    for (int index = 1; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }
    Options options;
    std::string error;
    if (!parse_options(args, options, error)) {
        deskfeed::console_write_stderr("deskfeed-check: " + error + "\n\n" + kUsage);
        return 1;
    }
    if (options.help) {
        deskfeed::console_write_stdout(kUsage);
        return 0;
    }

    std::string body;
    if (options.use_stdin) {
        if (!read_stdin(body, error)) {
            deskfeed::console_write_stderr("deskfeed-check: " + error + "\n");
            return 1;
        }
    } else if (!options.file.empty()) {
        bool exists = false;
        std::wstring read_error;
        if (!deskfeed::read_file_bytes(options.file, body, exists, read_error) || !exists) {
            deskfeed::console_write_stderr(
                "deskfeed-check: 读不到文件 " + deskfeed::utf8_from_wide(options.file) + "\n");
            return 1;
        }
    } else {
        deskfeed::HttpLimits limits;
        limits.timeout_ms = options.timeout_ms;
        deskfeed::HttpResult const result = deskfeed::http_get(options.url, limits);
        body = result.body;
        if (!result.ok) {
            std::string const kind = deskfeed::http_failure_name(result.failure);
            std::string const message = result.error.empty() ? std::string(kind) : result.error;
            if (options.report_json) {
                deskfeed::console_write_stdout(
                    report_json(false, kind.c_str(), message, body.size(), nullptr) + "\n");
            } else {
                print_human(false, kind.c_str(), message, body.size(), nullptr);
            }
            return 2;
        }
    }

    Snapshot snapshot;
    ValidationResult const validation = deskfeed::snapshot_validate(body, snapshot);
    if (options.report_json) {
        deskfeed::console_write_stdout(
            report_json(validation.ok, deskfeed::validation_kind_name(validation.kind),
                        validation.message, body.size(), validation.ok ? &snapshot : nullptr) +
            "\n");
    } else {
        print_human(validation.ok, deskfeed::validation_kind_name(validation.kind),
                    validation.message, body.size(), validation.ok ? &snapshot : nullptr);
    }
    return validation.ok ? 0 : 2;
}
