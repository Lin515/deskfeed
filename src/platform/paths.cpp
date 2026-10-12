#include "platform/paths.h"

#include "platform/win32_util.h"

#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

#include <cstdint>
#include <vector>

namespace deskfeed {
namespace {

std::wstring known_folder(REFKNOWNFOLDERID id, wchar_t const* environment_fallback) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &raw)) && raw != nullptr) {
        std::wstring result(raw);
        ::CoTaskMemFree(raw);
        if (!result.empty()) {
            return result;
        }
    }
    if (environment_fallback != nullptr) {
        wchar_t buffer[MAX_PATH * 2]{};
        DWORD const chars =
            ::GetEnvironmentVariableW(environment_fallback, buffer, static_cast<DWORD>(std::size(buffer)));
        if (chars > 0 && chars < std::size(buffer)) {
            return std::wstring(buffer, chars);
        }
    }
    return std::wstring();
}

// FNV-1a 64：只用来把 SID 压成短后缀，不承担任何安全职责。
std::uint64_t fnv1a(std::wstring const& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (wchar_t const ch : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned short>(ch));
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::wstring hex8(std::uint64_t value) {
    static wchar_t const digits[] = L"0123456789abcdef";
    std::wstring out(8, L'0');
    for (int index = 7; index >= 0; --index) {
        out[static_cast<std::size_t>(index)] = digits[value & 0xF];
        value >>= 4;
    }
    return out;
}

std::wstring read_environment(std::wstring const& name) {
    DWORD const needed = ::GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (needed == 0) {
        return std::wstring();
    }
    // needed 含结尾的空字符；按实际长度动态取，超长值也要原样读出来，
    // 好让上层按“非法实例名”拒绝，而不是悄悄退回生产命名空间。
    std::wstring buffer(needed, L'\0');
    DWORD const chars = ::GetEnvironmentVariableW(name.c_str(), buffer.data(), needed);
    if (chars == 0) {
        return std::wstring();
    }
    buffer.resize(chars);
    return buffer;
}

}  // namespace

std::wstring instance_suffix() {
    std::wstring const scope = read_environment(L"DESKFEED_SCOPE");
    if (!scope.empty()) {
        return scope;
    }
    return read_environment(L"DESKFEED_TEST_INSTANCE");
}

bool scope_validate(std::wstring const& scope, std::wstring& error) {
    if (scope.empty()) {
        return true;  // 生产实例
    }
    if (scope.size() > 32) {
        error = L"实例名最长 32 个字符";
        return false;
    }
    for (wchar_t const ch : scope) {
        bool const allowed = (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') ||
                             (ch >= L'A' && ch <= L'Z') || ch == L'-' || ch == L'_';
        if (!allowed) {
            error = L"实例名只允许字母、数字、连字符与下划线";
            return false;
        }
    }
    return true;
}

std::wstring current_user_sid() {
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return std::wstring();
    }
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<unsigned char> buffer(needed == 0 ? 256 : needed);
    std::wstring result;
    if (::GetTokenInformation(token, TokenUser, buffer.data(), static_cast<DWORD>(buffer.size()),
                              &needed) != FALSE) {
        auto const* user = reinterpret_cast<TOKEN_USER const*>(buffer.data());
        LPWSTR text = nullptr;
        if (::ConvertSidToStringSidW(user->User.Sid, &text) != FALSE && text != nullptr) {
            result.assign(text);
            ::LocalFree(text);
        }
    }
    ::CloseHandle(token);
    return result;
}

std::wstring scoped_name(std::wstring const& base, std::wstring const& suffix) {
    std::wstring name = base;
    if (!suffix.empty()) {
        name += L"-test-" + suffix;
    }
    std::wstring const sid = current_user_sid();
    if (!sid.empty()) {
        name += L"-" + hex8(fnv1a(sid));
    }
    return name;
}

std::wstring deskfeed_settings_path(std::wstring const& suffix) {
    std::wstring base = known_folder(FOLDERID_RoamingAppData, L"APPDATA");
    if (base.empty()) {
        base = L".";
    }
    base += L"\\DeskFeed";
    if (!suffix.empty()) {
        base += L"\\test-" + suffix;
    }
    base += L"\\settings.ini";
    return base;
}

std::wstring deskfeed_log_path(std::wstring const& suffix) {
    std::wstring base = known_folder(FOLDERID_LocalAppData, L"LOCALAPPDATA");
    if (base.empty()) {
        base = L".";
    }
    base += L"\\DeskFeed";
    if (!suffix.empty()) {
        base += L"\\test-" + suffix;
    }
    base += L"\\deskfeed.log";
    return base;
}

std::wstring deskfeed_runtime_dir(std::wstring const& suffix) {
    std::wstring base = known_folder(FOLDERID_LocalAppData, L"LOCALAPPDATA");
    if (base.empty()) {
        base = L".";
    }
    base += L"\\DeskFeed";
    if (!suffix.empty()) {
        base += L"\\test-" + suffix;
    }
    return base;
}

std::wstring deskfeed_inspect_path(std::wstring const& suffix, unsigned long sender_pid) {
    return deskfeed_runtime_dir(suffix) + L"\\inspect-" + std::to_wstring(sender_pid) + L".json";
}

bool ensure_parent_directory(std::wstring const& file_path, std::wstring& error) {
    std::size_t const slash = file_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return true;
    }
    std::wstring const directory = file_path.substr(0, slash);
    int const result = ::SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    if (result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS) {
        return true;
    }
    error = L"创建目录失败 " + directory + L"：" + last_error_text(static_cast<DWORD>(result));
    return false;
}

bool write_file_bytes(std::wstring const& path, std::string const& bytes, std::wstring& error) {
    if (!ensure_parent_directory(path, error)) {
        return false;
    }
    HANDLE const file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"建不了文件 " + path + L"：" + last_error_text();
        return false;
    }
    bool ok = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        DWORD const want = static_cast<DWORD>(bytes.size() - offset);
        if (::WriteFile(file, bytes.data() + offset, want, &written, nullptr) == FALSE) {
            error = L"写文件失败：" + last_error_text();
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && ::FlushFileBuffers(file) == FALSE) {
        error = L"刷盘失败：" + last_error_text();
        ok = false;
    }
    ::CloseHandle(file);
    if (!ok) {
        ::DeleteFileW(path.c_str());
    }
    return ok;
}

bool read_file_bytes(std::wstring const& path, std::string& bytes, bool& exists, std::wstring& error) {
    bytes.clear();
    HANDLE const file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD const code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            exists = false;
            return true;
        }
        error = L"打不开文件 " + path + L"：" + last_error_text(code);
        return false;
    }
    exists = true;
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) == FALSE) {
        error = L"读不了文件大小：" + last_error_text();
        ::CloseHandle(file);
        return false;
    }
    if (size.QuadPart > 4 * 1024 * 1024) {
        error = L"文件超过 4 MiB，拒绝读取：" + path;
        ::CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        DWORD const want = static_cast<DWORD>(bytes.size() - offset);
        if (::ReadFile(file, bytes.data() + offset, want, &read, nullptr) == FALSE) {
            error = L"读文件失败：" + last_error_text();
            ::CloseHandle(file);
            return false;
        }
        if (read == 0) {
            break;
        }
        offset += read;
    }
    bytes.resize(offset);
    ::CloseHandle(file);
    return true;
}

bool delete_file(std::wstring const& path) { return ::DeleteFileW(path.c_str()) != FALSE; }

}  // namespace deskfeed
