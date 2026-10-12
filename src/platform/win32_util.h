#pragma once

#include <windows.h>

#include <string>

// Small Win32 helpers used for diagnostics in the injector and the payload.
namespace deskfeed {

std::wstring hex_text(unsigned long long value);

// "0x80070005 (Access is denied.)" - the numeric value is always included so a
// missing system message never hides which code came back.
std::wstring hresult_text(HRESULT result);

std::wstring last_error_text(DWORD error);

// Values from GetLastError.
std::wstring last_error_text();

std::wstring window_class_name(HWND window);
std::wstring window_text(HWND window);
std::wstring window_rect_text(HWND window);
unsigned long window_process_id(HWND window);

// "Shell_TrayWnd 0x00010162 0,1392 2560x48 V popup"
std::wstring describe_window(HWND window);

HWND find_top_level_window(std::wstring const& class_name);
HWND find_child_window(HWND parent, std::wstring const& class_name);

// Rectangle of a window in the client coordinates of another window.
RECT window_rect_in_client(HWND window, HWND client_owner);

std::wstring module_file_path(HMODULE module);

// "<directory>\<name>", tolerating a trailing separator in the directory and an
// empty directory (which yields the bare name).
std::wstring join_path(std::wstring const& directory, std::wstring const& name);

// Path of the module that contains the calling code. Works for an injected DLL
// without it having to remember its own HINSTANCE.
std::wstring module_self_path();

// Handle of the module that contains the calling code.
HMODULE module_self_handle();

}  // namespace deskfeed
