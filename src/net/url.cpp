#include "net/url.h"

#include "platform/win32_util.h"

#include <windows.h>
#include <winhttp.h>

#include <cwctype>
#include <vector>

namespace deskfeed {
namespace {

// 去掉查询串与片段。只在“相对引用要基于基地址的路径”这一步用得到：
// 解析出的最终地址保留查询串，只丢片段（片段不发给服务器）。
std::wstring strip_query_and_fragment(std::wstring_view path) {
    std::size_t const cut = path.find_first_of(L"?#");
    return std::wstring(cut == std::wstring_view::npos ? path : path.substr(0, cut));
}

std::wstring strip_fragment(std::wstring_view path) {
    std::size_t const cut = path.find(L'#');
    return std::wstring(cut == std::wstring_view::npos ? path : path.substr(0, cut));
}

// RFC 3986 的 remove_dot_segments。调用方已经先把查询串摘掉了。
std::wstring remove_dot_segments(std::wstring const& path) {
    bool const absolute = !path.empty() && path.front() == L'/';
    bool trailing_slash = !path.empty() && path.back() == L'/';

    std::vector<std::wstring> segments;
    std::wstring current;
    for (wchar_t const ch : path) {
        if (ch == L'/') {
            segments.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    segments.push_back(current);

    std::vector<std::wstring> out;
    for (std::size_t index = 0; index < segments.size(); ++index) {
        std::wstring const& segment = segments[index];
        if (segment == L".") {
            trailing_slash = trailing_slash || index + 1 == segments.size();
            continue;
        }
        if (segment == L"..") {
            // RFC 3986：删掉输出里的最后一段。中间的空段（连续斜杠产生的）也算一段，
            // 但代表根的那个前导空段不能删，否则 "/.." 会变成空串而不是 "/"。
            bool const only_root_marker = out.size() <= 1 && !out.empty() && out.front().empty();
            if (!out.empty() && !only_root_marker) {
                out.pop_back();
            }
            trailing_slash = trailing_slash || index + 1 == segments.size();
            continue;
        }
        out.push_back(segment);
    }

    std::wstring result;
    for (std::size_t index = 0; index < out.size(); ++index) {
        if (index > 0) {
            result.push_back(L'/');
        }
        result += out[index];
    }
    if (absolute && result.empty()) {
        result = L"/";
    } else if (trailing_slash && !result.empty() && result.back() != L'/') {
        result.push_back(L'/');
    }
    return result;
}

bool is_absolute_reference(std::wstring const& reference) {
    // RFC 3986 的 scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) 后面跟冒号。
    // 必须从第 0 个字符起算：`?next=https://example.com` 与 `/next?target=http://x`
    // 都是相对引用，只在字符串中间出现 "://" 不代表它是绝对地址。
    if (reference.empty()) {
        return false;
    }
    wchar_t const first = reference.front();
    bool const alpha = (first >= L'a' && first <= L'z') || (first >= L'A' && first <= L'Z');
    if (!alpha) {
        return false;
    }
    for (std::size_t index = 1; index < reference.size(); ++index) {
        wchar_t const ch = reference[index];
        bool const scheme_char = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
                                 (ch >= L'0' && ch <= L'9') || ch == L'+' || ch == L'-' ||
                                 ch == L'.';
        if (ch == L':') {
            return true;
        }
        if (!scheme_char) {
            return false;
        }
    }
    return false;
}

std::wstring scheme_of(std::wstring const& url) {
    std::size_t const end = url.find(L"://");
    return end == std::wstring::npos ? std::wstring() : url.substr(0, end);
}

}  // namespace

std::wstring url_redact(std::wstring const& url, bool* removed) {
    if (removed != nullptr) {
        *removed = false;
    }
    std::size_t const scheme_end = url.find(L"://");
    if (scheme_end == std::wstring::npos) {
        // 认不出来的地址：宁可把整条换成一个占位符，也不要把可能的凭据漏出去。
        if (!url.empty() && removed != nullptr) {
            *removed = true;
        }
        return url.empty() ? url : std::wstring(L"<unsupported-url>");
    }
    std::size_t const authority_start = scheme_end + 3;
    std::size_t authority_end = url.find_first_of(L"/?#", authority_start);
    if (authority_end == std::wstring::npos) {
        authority_end = url.size();
    }
    std::wstring authority = url.substr(authority_start, authority_end - authority_start);

    bool changed = false;
    std::size_t const at = authority.rfind(L'@');
    if (at != std::wstring::npos) {
        authority = L"***@" + authority.substr(at + 1);
        changed = true;
    }

    std::wstring path = authority_end < url.size() ? url.substr(authority_end) : std::wstring();
    std::size_t const cut = path.find_first_of(L"?#");
    if (cut != std::wstring::npos) {
        changed = true;
        path.resize(cut);
    }
    if (removed != nullptr) {
        *removed = changed;
    }
    return url.substr(0, authority_start) + authority + path;
}

bool url_crack(std::wstring const& url, UrlParts& out, std::wstring& error) {
    if (url.empty()) {
        error = L"地址是空的";
        return false;
    }
    // 地址按完整输入字符串使用：**不做**去空白、不做任何归一化。
    // 首尾空白、内部空白与控制字符一律当非法地址拒绝，避免“写进去的和请求的不是同一条”。
    for (wchar_t const ch : url) {
        if (ch <= 0x20 || ch == 0x7F) {
            error = L"地址里有空白或控制字符；地址必须原样合法（需要空格请用 %20）";
            return false;
        }
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUserNameLength = static_cast<DWORD>(-1);
    parts.dwPasswordLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) == FALSE) {
        error = L"地址解析失败：" + url_redact(url);
        return false;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"只接受 http / https 地址：" + url_redact(url);
        return false;
    }
    if (parts.dwHostNameLength == 0 || parts.lpszHostName == nullptr) {
        error = L"地址里没有主机名：" + url_redact(url);
        return false;
    }

    UrlParts result;
    result.scheme = parts.nScheme == INTERNET_SCHEME_HTTPS ? L"https" : L"http";
    result.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    result.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    result.port = parts.nPort;
    if (parts.dwUrlPathLength > 0 && parts.lpszUrlPath != nullptr) {
        result.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    }
    if (parts.dwExtraInfoLength > 0 && parts.lpszExtraInfo != nullptr) {
        result.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    // 只丢片段；**查询串必须原样保留**——配置里写了查询串，请求就要带查询串。
    result.path = strip_fragment(result.path);
    if (result.path.empty()) {
        result.path = L"/";
    } else if (result.path.front() != L'/') {
        result.path.insert(result.path.begin(), L'/');
    }

    out = result;
    return true;
}

bool url_resolve(std::wstring const& base, std::wstring const& reference, std::wstring& out,
                 std::wstring& error) {
    if (reference.empty()) {
        error = L"重定向目标为空";
        return false;
    }

    std::wstring resolved;
    if (is_absolute_reference(reference)) {
        resolved = reference;  // 绝对地址
    } else if (reference.rfind(L"//", 0) == 0) {
        std::wstring const scheme = scheme_of(base);
        if (scheme.empty()) {
            error = L"基地址没有协议，无法解析网络路径引用";
            return false;
        }
        resolved = scheme + L":" + reference;
    } else {
        std::size_t const scheme_end = base.find(L"://");
        if (scheme_end == std::wstring::npos) {
            error = L"基地址没有协议";
            return false;
        }
        std::size_t const authority_end = base.find_first_of(L"/?#", scheme_end + 3);
        std::wstring const prefix =
            authority_end == std::wstring::npos ? base : base.substr(0, authority_end);
        std::wstring const base_path =
            authority_end == std::wstring::npos ? std::wstring() : base.substr(authority_end);

        if (reference.front() == L'/') {
            resolved = prefix + reference;
        } else if (reference.front() == L'?') {
            resolved = prefix + strip_query_and_fragment(base_path) + reference;
        } else if (reference.front() == L'#') {
            resolved = prefix + base_path;
        } else {
            std::wstring const path = strip_query_and_fragment(base_path);
            std::size_t const slash = path.find_last_of(L'/');
            std::wstring const directory =
                slash == std::wstring::npos ? std::wstring(L"/") : path.substr(0, slash + 1);
            resolved = prefix + directory + reference;
        }
    }

    // 查询串不参与点段合并，先摘下来最后再拼回去。
    std::wstring query;
    std::size_t const query_at = resolved.find(L'?');
    std::wstring head = resolved;
    if (query_at != std::wstring::npos) {
        query = resolved.substr(query_at);
        head.resize(query_at);
    }
    std::size_t const fragment_at = head.find(L'#');
    if (fragment_at != std::wstring::npos) {
        head.resize(fragment_at);
    }

    std::size_t const head_scheme = head.find(L"://");
    if (head_scheme != std::wstring::npos) {
        std::size_t authority_start = head_scheme + 3;
        std::size_t authority_stop = head.find_first_of(L"/?#", authority_start);
        if (authority_stop == std::wstring::npos) {
            head = head + L"/";
        } else {
            std::wstring const prefix = head.substr(0, authority_stop);
            std::wstring const path = head.substr(authority_stop);
            head = prefix + remove_dot_segments(path);
        }
    }
    std::wstring const candidate = head + query;

    UrlParts parts;
    if (!url_crack(candidate, parts, error)) {
        return false;
    }
    out = candidate;
    return true;
}

}  // namespace deskfeed
