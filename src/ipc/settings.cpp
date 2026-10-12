#include "ipc/settings.h"

#include "core/utf8.h"
#include "platform/paths.h"
#include "platform/win32_util.h"

#include <windows.h>

#include <cwctype>
#include <iterator>

namespace deskfeed {
namespace {

bool equals_ci(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::towlower(left[index]) != std::towlower(right[index])) {
            return false;
        }
    }
    return true;
}

std::wstring_view trim(std::wstring_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool read_file(std::wstring const& path, std::string& bytes, bool& exists, std::wstring& error) {
    exists = false;
    HANDLE const file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD const code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        error = L"打不开设置文件 " + path + L"：" + last_error_text(code);
        return false;
    }
    exists = true;
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) == FALSE) {
        error = L"读不了设置文件大小：" + last_error_text();
        ::CloseHandle(file);
        return false;
    }
    if (size.QuadPart > 1024 * 1024) {
        error = L"设置文件超过 1 MiB，拒绝读取";
        ::CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        DWORD const want = static_cast<DWORD>(bytes.size() - offset);
        if (::ReadFile(file, bytes.data() + offset, want, &read, nullptr) == FALSE) {
            error = L"读设置文件失败：" + last_error_text();
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

}  // namespace

SettingsFile::SettingsFile(std::wstring path) : path_(std::move(path)) {}

bool SettingsFile::load(std::wstring& error) {
    std::string bytes;
    bool exists = false;
    if (!read_file(path_, bytes, exists, error)) {
        return false;
    }
    if (!exists) {
        text_.clear();
        lines_.clear();
        loaded_ = true;
        return true;
    }
    // 允许 UTF-8 BOM。
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    if (!utf8_validate(bytes)) {
        error = L"设置文件不是合法 UTF-8：" + path_;
        return false;
    }
    text_ = wide_from_utf8(bytes);
    parse();
    loaded_ = true;
    return true;
}

void SettingsFile::parse() {
    lines_.clear();
    std::wstring current_section;

    std::vector<std::wstring> raw_lines;
    std::wstring line;
    for (wchar_t const ch : text_) {
        if (ch == L'\n') {
            if (!line.empty() && line.back() == L'\r') {
                line.pop_back();
            }
            raw_lines.push_back(line);
            line.clear();
        } else {
            line.push_back(ch);
        }
    }
    if (!line.empty()) {
        raw_lines.push_back(line);
    }

    for (auto const& raw : raw_lines) {
        Line entry;
        entry.raw = raw;
        std::wstring_view const view = trim(raw);
        if (!view.empty() && view.front() == L'[') {
            std::size_t const close = view.find(L']');
            if (close != std::wstring_view::npos) {
                entry.is_section = true;
                entry.section = std::wstring(trim(view.substr(1, close - 1)));
                current_section = entry.section;
            }
        } else if (!view.empty() && view.front() != L';' && view.front() != L'#') {
            std::size_t const equals = view.find(L'=');
            if (equals != std::wstring_view::npos) {
                entry.is_pair = true;
                entry.key = std::wstring(trim(view.substr(0, equals)));
                // 值按文件里的原样保留（只去掉行尾的 CR）。地址必须精确：这里不做
                // 任何去空白或大小写处理，写坏了就由使用方按“非法地址”拒绝并报出来。
                entry.value = std::wstring(view.substr(equals + 1));
            }
        }
        if (!entry.is_section) {
            entry.section = current_section;
        }
        lines_.push_back(std::move(entry));
    }
}

std::wstring SettingsFile::render() const {
    std::wstring out;
    for (auto const& line : lines_) {
        out += line.raw;
        out += L"\r\n";
    }
    return out;
}

bool SettingsFile::get(std::wstring_view section, std::wstring_view key, std::wstring& value) const {
    for (auto const& line : lines_) {
        if (line.is_pair && equals_ci(line.section, section) && equals_ci(line.key, key)) {
            value = line.value;
            return true;
        }
    }
    return false;
}

long long SettingsFile::get_integer(std::wstring_view section, std::wstring_view key,
                                    long long fallback) const {
    std::wstring text;
    if (!get(section, key, text) || text.empty()) {
        return fallback;
    }
    // 数值键允许首尾空白（人写的文件里常见），字符串键则严格按原样。
    std::wstring const cleaned(trim(text));
    wchar_t* end = nullptr;
    long long const value = std::wcstoll(cleaned.c_str(), &end, 10);
    if (end == cleaned.c_str() || *end != L'\0') {
        return fallback;
    }
    return value;
}

bool SettingsFile::set(std::wstring_view section, std::wstring_view key,
                       std::wstring const& value, bool* changed, std::wstring& error) {
    if (changed != nullptr) {
        *changed = true;
    }
    for (auto& line : lines_) {
        if (line.is_pair && equals_ci(line.section, section) && equals_ci(line.key, key)) {
            if (changed != nullptr) {
                *changed = line.value != value;
            }
            line.value = value;
            line.raw = std::wstring(key) + L"=" + value;
            return true;
        }
    }

    // 键不存在：插到该栏目的最后一行之后；栏目不存在就先补一行栏目头。
    Line entry;
    entry.section = std::wstring(section);
    entry.is_pair = true;
    entry.key = std::wstring(key);
    entry.value = value;
    entry.raw = std::wstring(key) + L"=" + value;

    std::size_t insert_at = lines_.size();
    std::size_t section_header = lines_.size();
    for (std::size_t index = 0; index < lines_.size(); ++index) {
        if (lines_[index].is_section && equals_ci(lines_[index].section, section)) {
            section_header = index;
            insert_at = index + 1;
        } else if (section_header != lines_.size() && lines_[index].is_section) {
            break;  // 到了下一个栏目
        } else if (section_header != lines_.size()) {
            insert_at = index + 1;
        }
    }
    if (section_header == lines_.size()) {
        if (!lines_.empty() && !lines_.back().raw.empty()) {
            Line blank;
            blank.raw.clear();
            lines_.push_back(std::move(blank));
        }
        Line header;
        header.is_section = true;
        header.section = entry.section;
        header.raw = L"[" + entry.section + L"]";
        lines_.push_back(std::move(header));
        insert_at = lines_.size();
    }
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(insert_at), std::move(entry));
    (void)error;
    return true;
}

bool SettingsFile::save(std::wstring& error) {
    if (!save_as(path_, error)) {
        return false;
    }
    // 让内存里的行与磁盘一致（值已经在里面，这一步只是重排 raw）。
    load(error);
    return true;
}

bool SettingsFile::save_as(std::wstring const& path, std::wstring& error) {
    if (!ensure_parent_directory(path, error)) {
        return false;
    }
    std::string const bytes = utf8_from_wide(render());
    std::wstring const temporary = path + L".tmp-" + std::to_wstring(::GetCurrentProcessId());

    HANDLE const file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"建不了临时设置文件 " + temporary + L"：" + last_error_text();
        return false;
    }
    std::size_t offset = 0;
    bool ok = true;
    while (offset < bytes.size()) {
        DWORD written = 0;
        DWORD const want = static_cast<DWORD>(bytes.size() - offset);
        if (::WriteFile(file, bytes.data() + offset, want, &written, nullptr) == FALSE) {
            error = L"写临时设置文件失败：" + last_error_text();
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
        ::DeleteFileW(temporary.c_str());
        return false;
    }
    if (::MoveFileExW(temporary.c_str(), path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        error = L"替换设置文件失败：" + last_error_text();
        ::DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

void SettingsFile::adopt(SettingsFile const& committed) {
    path_ = committed.path_;
    text_ = committed.text_;
    lines_ = committed.lines_;
    loaded_ = committed.loaded_;
}

SourceSettings SettingsFile::read_source() const {
    SourceSettings source;
    std::wstring text;
    if (get(L"source", L"url", text)) {
        source.url = text;
    }
    source.poll_ms = get_integer(L"source", L"poll_ms", 0);
    source.retry_ms = get_integer(L"source", L"retry_ms", 0);
    source.timeout_ms = get_integer(L"source", L"timeout_ms", 0);
    return source;
}

bool SettingsFile::write_source_url(std::wstring const& url, bool* changed, std::wstring& error) {
    if (!set(L"source", L"url", url, changed, error)) {
        return false;
    }
    return save(error);
}

}  // namespace deskfeed
