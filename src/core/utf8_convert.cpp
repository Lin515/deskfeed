#include "core/utf8.h"

#include <windows.h>

// UTF-8 与 UTF-16 的互转。这里刻意与 core/utf8.cpp 的**校验器**分成两个翻译单元：
//
// 任务栏负载 DLL 要跑在 explorer.exe 里，它的审计要求是“最终模块里没有 JSON、快照、
// 协议校验器这些主进程才需要的东西”。邮箱的消费者只需要把 Win32 的错误文本换成 UTF-8，
// 渲染侧又只需要把协议文本换成宽字符，所以负载只链这一份转换代码
// （CMake 目标 `deskfeed_core_text`），`utf8_validate` / `is_blank` / `has_line_break`
// 这些校验器留在 `core/utf8.cpp`，一个都不会被拉进 explorer。
//
// 声明仍在 `core/utf8.h`：调用方（主进程、IPC、网络层、显示层）看到的接口没有任何变化。
namespace deskfeed {
namespace {

std::string to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return std::string();
    }
    int const bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return std::string();
    }
    std::string result(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                          bytes, nullptr, nullptr);
    return result;
}

std::wstring to_wide(std::string_view text) {
    if (text.empty()) {
        return std::wstring();
    }
    int const chars = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (chars <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<std::size_t>(chars), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(), chars);
    return result;
}

}  // namespace

std::wstring wide_from_utf8(std::string_view text) { return to_wide(text); }

std::string utf8_from_wide(std::wstring_view text) { return to_utf8(text); }

}  // namespace deskfeed
