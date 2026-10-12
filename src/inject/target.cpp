#include "inject/target.h"

#include "platform/paths.h"
#include "platform/win32_util.h"

#include <iterator>

namespace deskfeed::inject {

unsigned long taskbar_owner_pid() {
    wchar_t const* const names[] = {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"};
    for (wchar_t const* name : names) {
        HWND const window = ::FindWindowW(name, nullptr);
        if (window == nullptr) {
            continue;
        }
        DWORD pid = 0;
        ::GetWindowThreadProcessId(window, &pid);
        if (pid != 0) {
            return pid;
        }
    }
    return 0;
}

bool resolve_taskbar_owner(ProcessEntry& out, std::wstring& error) {
    out = ProcessEntry{};
    unsigned long const owner = taskbar_owner_pid();
    if (owner == 0) {
        error = L"the taskbar owner was not found (neither Shell_TrayWnd nor Shell_SecondaryTrayWnd "
                L"exists right now); refusing to guess a process";
        return false;
    }

    out.process_id = owner;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner);
    if (process != nullptr) {
        wchar_t buffer[MAX_PATH * 2]{};
        DWORD chars = static_cast<DWORD>(std::size(buffer));
        if (::QueryFullProcessImageNameW(process, 0, buffer, &chars)) {
            out.image_path.assign(buffer, chars);
        }
        ::CloseHandle(process);
    }

    // PID 会被复用，所以"还是当初那个进程"的判断要带上创建时间。取不到就保持 0，调用方自己决定
    // 是否继续（本工具的所有写操作前都会再核对一次）。
    std::uint64_t creation_time = 0;
    std::wstring time_error;
    if (process_creation_time(owner, creation_time, time_error)) {
        out.creation_time = creation_time;
    }
    return true;
}

bool resolve_scope(std::wstring const& text, std::wstring& scope, std::wstring& error) {
    scope.clear();
    error.clear();
    if (text.empty()) {
        return true;  // 生产实例
    }
    if (!deskfeed::scope_validate(text, error)) {
        return false;
    }
    scope = text;
    return true;
}

}  // namespace deskfeed::inject
