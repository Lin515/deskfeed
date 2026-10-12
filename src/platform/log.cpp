#include "platform/log.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>

namespace deskfeed {
namespace {

std::mutex g_mutex;
HANDLE g_file = INVALID_HANDLE_VALUE;
bool g_mirror_console = false;

void write_raw(HANDLE handle, void const* data, DWORD bytes) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || bytes == 0) {
        return;
    }
    DWORD written = 0;
    ::WriteFile(handle, data, bytes, &written, nullptr);
}

std::string to_utf8(std::wstring const& text) {
    if (text.empty()) {
        return std::string();
    }
    int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return std::string();
    }
    std::string result(static_cast<size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          result.data(), bytes, nullptr, nullptr);
    return result;
}

std::wstring timestamp_text() {
    SYSTEMTIME local{};
    ::GetLocalTime(&local);
    wchar_t buffer[32]{};
    ::swprintf_s(buffer, L"%02u:%02u:%02u.%03u",
                 static_cast<unsigned>(local.wHour),
                 static_cast<unsigned>(local.wMinute),
                 static_cast<unsigned>(local.wSecond),
                 static_cast<unsigned>(local.wMilliseconds));
    return buffer;
}

void close_locked() {
    if (g_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_mirror_console = false;
}

}  // namespace

bool log_open(std::wstring const& path, bool mirror_to_console) {
    std::lock_guard<std::mutex> guard(g_mutex);
    close_locked();

    // Make sure the parent directory exists; SHCreateDirectoryExW is happy when
    // the directory is already there.
    std::wstring::size_type slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        ::SHCreateDirectoryExW(nullptr, path.substr(0, slash).c_str(), nullptr);
    }

    g_file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    // Console mirroring is decided by the caller, not by whether the log file
    // could be opened: the injector must still report what it is doing when its
    // log file is unavailable.
    g_mirror_console = mirror_to_console;

    if (g_file == INVALID_HANDLE_VALUE) {
        return false;
    }

    // UTF-8 so the log file can be read by any editor or tool.
    unsigned char const bom[3] = {0xEF, 0xBB, 0xBF};
    write_raw(g_file, bom, sizeof(bom));
    return true;
}

void log_close() {
    std::lock_guard<std::mutex> guard(g_mutex);
    close_locked();
}

bool log_is_open() {
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_file != INVALID_HANDLE_VALUE;
}

void log_line(std::wstring const& text) {
    std::lock_guard<std::mutex> guard(g_mutex);
    bool to_file = g_file != INVALID_HANDLE_VALUE;
    if (!to_file && !g_mirror_console) {
        // Last resort so an unopenable log file never hides a failure.
        std::wstring debug_line = text + L"\r\n";
        ::OutputDebugStringW(debug_line.c_str());
        return;
    }

    std::wstring line = L"[" + timestamp_text() + L" t" +
                        std::to_wstring(::GetCurrentThreadId()) + L"] " + text + L"\r\n";
    std::string utf8 = to_utf8(line);
    write_raw(g_file, utf8.data(), static_cast<DWORD>(utf8.size()));

    if (g_mirror_console) {
        HANDLE target = ::GetStdHandle(STD_ERROR_HANDLE);
        DWORD mode = 0;
        if (target != nullptr && target != INVALID_HANDLE_VALUE &&
            ::GetConsoleMode(target, &mode)) {
            DWORD written = 0;
            ::WriteConsoleW(target, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        } else {
            // Redirected console: emit UTF-8 so the bytes stay readable.
            write_raw(target, utf8.data(), static_cast<DWORD>(utf8.size()));
        }
    }
}

void log_format(wchar_t const* format, ...) {
    wchar_t stack_buffer[1024]{};
    va_list args;
    va_start(args, format);
    int needed = ::_vscwprintf(format, args);
    va_end(args);

    if (needed < 0) {
        log_line(L"<log_format failed>");
        return;
    }
    if (needed < static_cast<int>(std::size(stack_buffer))) {
        va_start(args, format);
        ::_vsnwprintf_s(stack_buffer, std::size(stack_buffer), _TRUNCATE, format, args);
        va_end(args);
        log_line(stack_buffer);
        return;
    }

    std::wstring heap_buffer(static_cast<size_t>(needed) + 1, L'\0');
    va_start(args, format);
    ::_vsnwprintf_s(heap_buffer.data(), heap_buffer.size(), _TRUNCATE, format, args);
    va_end(args);
    heap_buffer.resize(::wcslen(heap_buffer.c_str()));
    log_line(heap_buffer);
}

std::wstring default_log_path(wchar_t const* file_name) {
    std::wstring base;
    wchar_t buffer[MAX_PATH * 2]{};
    DWORD chars = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                            static_cast<DWORD>(std::size(buffer)));
    if (chars > 0 && chars < std::size(buffer)) {
        base.assign(buffer, chars);
        base += L"\\DeskFeed\\";
    } else {
        base = L".\\";
    }
    base += file_name;
    return base;
}

}  // namespace deskfeed
