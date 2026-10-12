#include "platform/user_scope.h"

#include "platform/paths.h"

#include <sddl.h>

namespace deskfeed {
namespace {

std::wstring token_user_sid(HANDLE token) {
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<unsigned char> buffer(needed == 0 ? 256 : needed);
    if (::GetTokenInformation(token, TokenUser, buffer.data(), static_cast<DWORD>(buffer.size()),
                              &needed) == FALSE) {
        return std::wstring();
    }
    auto const* user = reinterpret_cast<TOKEN_USER const*>(buffer.data());
    LPWSTR text = nullptr;
    if (::ConvertSidToStringSidW(user->User.Sid, &text) == FALSE || text == nullptr) {
        return std::wstring();
    }
    std::wstring result(text);
    ::LocalFree(text);
    return result;
}

}  // namespace

bool UserSecurity::build() {
    std::wstring const sid_text = current_user_sid();
    if (sid_text.empty()) {
        return false;
    }
    // D: 自由访问控制列表；A 允许；GA 完全访问；只给当前用户。
    std::wstring const sddl = L"D:(A;;GA;;;" + sid_text + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr) == FALSE ||
        descriptor == nullptr) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(descriptor);
    // 描述符由 LocalAlloc 分配，按 SDDL 长度复制一份自己持有，随后释放原块。
    DWORD const length = ::GetSecurityDescriptorLength(descriptor);
    descriptor_.assign(bytes, bytes + length);
    ::LocalFree(descriptor);

    attributes_.nLength = sizeof(SECURITY_ATTRIBUTES);
    attributes_.lpSecurityDescriptor = descriptor_.data();
    attributes_.bInheritHandle = FALSE;
    ready_ = true;
    return true;
}

bool process_user_matches(std::uint32_t process_id) {
    if (process_id == 0 || process_id == ::GetCurrentProcessId()) {
        return process_id == ::GetCurrentProcessId();
    }
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        return false;
    }
    HANDLE token = nullptr;
    bool matches = false;
    if (::OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE) {
        std::wstring const other = token_user_sid(token);
        std::wstring const self = current_user_sid();
        matches = !other.empty() && other == self;
        ::CloseHandle(token);
    }
    ::CloseHandle(process);
    return matches;
}

}  // namespace deskfeed
