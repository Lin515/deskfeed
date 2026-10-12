#include "platform/console.h"

#include "core/utf8.h"

#include <windows.h>

#include <cstdio>

namespace deskfeed {
namespace {

// GUI 子系统的进程默认没有控制台。命令行用法（--help、--inspect、报错）要能看到输出，
// 所以在第一次需要写标准输出/错误、而系统又没给句柄时，挂到父进程的控制台上，
// 并把 Win32 句柄与 C 运行库流都指到 CONOUT$。重定向到文件/管道时句柄本来就有效，这一步不会发生。
HANDLE resolve_handle(DWORD std_id) {
    HANDLE handle = ::GetStdHandle(std_id);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        return handle;
    }
    static bool tried_attach = false;
    if (!tried_attach) {
        tried_attach = true;
        if (::AttachConsole(ATTACH_PARENT_PROCESS) != FALSE) {
            HANDLE const console = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                 OPEN_EXISTING, 0, nullptr);
            if (console != INVALID_HANDLE_VALUE) {
                ::SetStdHandle(STD_OUTPUT_HANDLE, console);
                ::SetStdHandle(STD_ERROR_HANDLE, console);
                FILE* stream = nullptr;
                freopen_s(&stream, "CONOUT$", "w", stdout);
                freopen_s(&stream, "CONOUT$", "w", stderr);
            }
        }
    }
    return ::GetStdHandle(std_id);
}

void write_handle(HANDLE handle, std::string const& bytes) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || bytes.empty()) {
        return;
    }
    DWORD written = 0;
    if (::GetFileType(handle) == FILE_TYPE_CHAR) {
        std::wstring const text = wide_from_utf8(bytes);
        if (!text.empty()) {
            ::WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        }
        return;
    }
    ::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

}  // namespace

void console_write_stdout(std::string const& utf8) {
    write_handle(resolve_handle(STD_OUTPUT_HANDLE), utf8);
}

void console_write_stderr(std::string const& utf8) {
    write_handle(resolve_handle(STD_ERROR_HANDLE), utf8);
}

void console_write_stderr(std::wstring const& text) { console_write_stderr(utf8_from_wide(text)); }

}  // namespace deskfeed
