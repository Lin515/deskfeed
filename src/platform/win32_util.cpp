#include "platform/win32_util.h"

#include <cwchar>
#include <iterator>

namespace deskfeed {
namespace {

std::wstring trim_message(std::wstring text) {
    while (!text.empty()) {
        wchar_t last = text.back();
        if (last == L'\r' || last == L'\n' || last == L' ' || last == L'\t' || last == L'.') {
            text.pop_back();
            continue;
        }
        break;
    }
    return text;
}

std::wstring system_message(DWORD code) {
    wchar_t* buffer = nullptr;
    DWORD chars = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text;
    if (chars != 0 && buffer != nullptr) {
        text = trim_message(std::wstring(buffer, chars));
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    return text;
}

}  // namespace

std::wstring hex_text(unsigned long long value) {
    wchar_t buffer[32]{};
    ::swprintf_s(buffer, L"0x%llX", value);
    return buffer;
}

std::wstring hresult_text(HRESULT result) {
    std::wstring text = hex_text(static_cast<unsigned long>(result));
    std::wstring message = system_message(static_cast<DWORD>(result));
    if (!message.empty()) {
        text += L" (" + message + L")";
    }
    return text;
}

std::wstring last_error_text(DWORD error) {
    std::wstring text = hex_text(error);
    std::wstring message = system_message(error);
    if (!message.empty()) {
        text += L" (" + message + L")";
    }
    return text;
}

std::wstring last_error_text() {
    return last_error_text(::GetLastError());
}

std::wstring window_class_name(HWND window) {
    wchar_t buffer[256]{};
    int chars = ::GetClassNameW(window, buffer, static_cast<int>(std::size(buffer)));
    return chars > 0 ? std::wstring(buffer, static_cast<size_t>(chars)) : std::wstring();
}

std::wstring window_text(HWND window) {
    wchar_t buffer[256]{};
    int chars = ::GetWindowTextW(window, buffer, static_cast<int>(std::size(buffer)));
    return chars > 0 ? std::wstring(buffer, static_cast<size_t>(chars)) : std::wstring();
}

std::wstring window_rect_text(HWND window) {
    RECT rect{};
    if (!::GetWindowRect(window, &rect)) {
        return L"<no rect>";
    }
    wchar_t buffer[128]{};
    ::swprintf_s(buffer, L"%d,%d %dx%d", rect.left, rect.top,
                 rect.right - rect.left, rect.bottom - rect.top);
    return buffer;
}

unsigned long window_process_id(HWND window) {
    DWORD process_id = 0;
    ::GetWindowThreadProcessId(window, &process_id);
    return process_id;
}

std::wstring describe_window(HWND window) {
    if (window == nullptr || !::IsWindow(window)) {
        return hex_text(reinterpret_cast<unsigned long long>(window)) + L" <not a window>";
    }

    wchar_t buffer[256]{};
    ::swprintf_s(buffer, L"%s %s %s %s %s",
                 window_class_name(window).c_str(),
                 hex_text(reinterpret_cast<unsigned long long>(window)).c_str(),
                 window_rect_text(window).c_str(),
                 ::IsWindowVisible(window) ? L"visible" : L"hidden",
                 (::GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) ? L"child" : L"popup");
    std::wstring text = buffer;
    std::wstring caption = window_text(window);
    if (!caption.empty()) {
        text += L" text='" + caption + L"'";
    }
    return text;
}

HWND find_top_level_window(std::wstring const& class_name) {
    return ::FindWindowW(class_name.c_str(), nullptr);
}

HWND find_child_window(HWND parent, std::wstring const& class_name) {
    return ::FindWindowExW(parent, nullptr, class_name.c_str(), nullptr);
}

RECT window_rect_in_client(HWND window, HWND client_owner) {
    RECT rect{};
    if (!::GetWindowRect(window, &rect)) {
        return RECT{};
    }
    POINT top_left{rect.left, rect.top};
    POINT bottom_right{rect.right, rect.bottom};
    ::ScreenToClient(client_owner, &top_left);
    ::ScreenToClient(client_owner, &bottom_right);
    RECT result{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
    return result;
}

std::wstring module_file_path(HMODULE module) {
    wchar_t buffer[MAX_PATH * 2]{};
    DWORD chars = ::GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
    return chars > 0 ? std::wstring(buffer, chars) : std::wstring();
}

HMODULE module_self_handle() {
    HMODULE module = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&module_self_handle), &module)) {
        return nullptr;
    }
    return module;
}

std::wstring module_self_path() {
    HMODULE module = module_self_handle();
    return module != nullptr ? module_file_path(module) : std::wstring();
}

std::wstring join_path(std::wstring const& directory, std::wstring const& name) {
    if (directory.empty()) {
        return name;
    }
    wchar_t last = directory[directory.size() - 1];
    if (last == L'\\' || last == L'/') {
        return directory + name;
    }
    return directory + L"\\" + name;
}

}  // namespace deskfeed
