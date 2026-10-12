#include "platform/pdb.h"

#include "platform/log.h"
#include "platform/win32_util.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <vector>

namespace deskfeed {
namespace {

// Read the whole file. The images involved are a few megabytes, so mapping adds
// nothing over one read.
bool read_file(std::wstring const& path, std::vector<unsigned char>& data, std::wstring& error) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"cannot open " + path + L": " + last_error_text();
        return false;
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        error = L"cannot size " + path + L": " + last_error_text();
        ::CloseHandle(file);
        return false;
    }

    data.resize(static_cast<size_t>(size.QuadPart));
    size_t total = 0;
    while (total < data.size()) {
        DWORD read = 0;
        DWORD chunk = static_cast<DWORD>(data.size() - total);
        if (!::ReadFile(file, data.data() + total, chunk, &read, nullptr)) {
            error = L"cannot read " + path + L": " + last_error_text();
            ::CloseHandle(file);
            return false;
        }
        if (read == 0) {
            break;
        }
        total += read;
    }
    ::CloseHandle(file);

    if (total != data.size()) {
        error = L"short read on " + path;
        return false;
    }
    return true;
}

unsigned long read_u32(std::vector<unsigned char> const& data, size_t offset) {
    unsigned long value = 0;
    ::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

unsigned short read_u16(std::vector<unsigned char> const& data, size_t offset) {
    unsigned short value = 0;
    ::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

struct Section {
    unsigned long virtual_address = 0;
    unsigned long raw_size = 0;
    unsigned long raw_pointer = 0;
};

// The debug directory is addressed by RVA, so the section table has to be walked
// to find the file offset that backs it.
bool rva_to_offset(std::vector<Section> const& sections, unsigned long rva, unsigned long& offset) {
    for (Section const& section : sections) {
        if (rva >= section.virtual_address && rva < section.virtual_address + section.raw_size) {
            offset = rva - section.virtual_address + section.raw_pointer;
            return true;
        }
    }
    return false;
}

// "<GUID><age>": Data1/2/3 are stored little-endian and stay in that order, the
// remaining eight bytes follow in order, all without dashes.
std::wstring format_identifier(unsigned char const* guid, unsigned long age) {
    wchar_t buffer[64] = {};
    unsigned long data1 = 0;
    unsigned short data2 = 0;
    unsigned short data3 = 0;
    ::memcpy(&data1, guid, sizeof(data1));
    ::memcpy(&data2, guid + 4, sizeof(data2));
    ::memcpy(&data3, guid + 6, sizeof(data3));

    int written = ::swprintf_s(buffer, L"%08lX%04X%04X", data1, data2, data3);
    for (int i = 8; i < 16; ++i) {
        written += ::swprintf_s(buffer + written, ARRAYSIZE(buffer) - written, L"%02X", guid[i]);
    }
    ::swprintf_s(buffer + written, ARRAYSIZE(buffer) - written, L"%lX", age);
    return buffer;
}

bool file_exists(std::wstring const& path) {
    DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ensure_directory(std::wstring const& path, std::wstring& error) {
    if (::CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        return true;
    }
    error = L"cannot create " + path + L": " + last_error_text();
    return false;
}

// One HTTPS GET of a binary resource, streamed to |target_path|.
bool http_download(std::wstring const& url, std::wstring const& target_path, std::wstring& error) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        error = L"cannot parse url " + url + L": " + last_error_text();
        return false;
    }

    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring resource(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    HINTERNET session = ::WinHttpOpen(L"DeskFeed/1.0",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        error = L"WinHttpOpen failed: " + last_error_text();
        return false;
    }

    bool ok = false;
    HINTERNET connection = ::WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    HINTERNET request = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    do {
        if (connection == nullptr) {
            error = L"WinHttpConnect failed: " + last_error_text();
            break;
        }

        request = ::WinHttpOpenRequest(connection, L"GET", resource.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (request == nullptr) {
            error = L"WinHttpOpenRequest failed: " + last_error_text();
            break;
        }

        if (!::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                                  0, 0, 0) ||
            !::WinHttpReceiveResponse(request, nullptr)) {
            error = L"symbol server request failed: " + last_error_text();
            break;
        }

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                   WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                   WINHTTP_NO_HEADER_INDEX)) {
            error = L"cannot read the response status: " + last_error_text();
            break;
        }
        if (status != 200) {
            error = L"symbol server answered " + std::to_wstring(status) + L" for " + url;
            break;
        }

        file = ::CreateFileW(target_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = L"cannot write " + target_path + L": " + last_error_text();
            break;
        }

        ok = true;
        for (;;) {
            DWORD available = 0;
            if (!::WinHttpQueryDataAvailable(request, &available)) {
                error = L"symbol download stalled: " + last_error_text();
                ok = false;
                break;
            }
            if (available == 0) {
                break;
            }

            std::vector<unsigned char> buffer(available);
            DWORD read = 0;
            if (!::WinHttpReadData(request, buffer.data(), available, &read)) {
                error = L"symbol download read failed: " + last_error_text();
                ok = false;
                break;
            }
            if (read == 0) {
                break;
            }

            DWORD written = 0;
            if (!::WriteFile(file, buffer.data(), read, &written, nullptr) || written != read) {
                error = L"cannot write the downloaded PDB: " + last_error_text();
                ok = false;
                break;
            }
        }
    } while (false);

    if (file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(file);
    }
    if (request != nullptr) {
        ::WinHttpCloseHandle(request);
    }
    if (connection != nullptr) {
        ::WinHttpCloseHandle(connection);
    }
    ::WinHttpCloseHandle(session);

    if (!ok) {
        ::DeleteFileW(target_path.c_str());
    }
    return ok;
}

}  // namespace

bool pdb_identity_from_image(std::wstring const& image_path,
                             std::wstring& pdb_name,
                             std::wstring& identifier,
                             std::wstring& error) {
    std::vector<unsigned char> data;
    if (!read_file(image_path, data, error)) {
        return false;
    }
    if (data.size() < 0x40) {
        error = image_path + L" is too small to be a PE image";
        return false;
    }

    size_t pe_offset = read_u32(data, 0x3C);
    if (pe_offset + 0x18 > data.size() || read_u32(data, pe_offset) != 0x00004550) {
        error = image_path + L" is not a PE image";
        return false;
    }

    size_t coff = pe_offset + 4;
    unsigned short section_count = read_u16(data, coff + 2);
    unsigned short optional_size = read_u16(data, coff + 16);
    size_t optional = coff + 20;
    if (read_u16(data, optional) != 0x20B) {
        error = image_path + L" is not a PE32+ image";
        return false;
    }

    std::vector<Section> sections;
    size_t section_table = optional + optional_size;
    for (unsigned short i = 0; i < section_count; ++i) {
        size_t header = section_table + static_cast<size_t>(i) * 40;
        if (header + 40 > data.size()) {
            error = L"section table of " + image_path + L" runs past the end of the file";
            return false;
        }
        Section section;
        section.virtual_address = read_u32(data, header + 12);
        section.raw_size = read_u32(data, header + 16);
        section.raw_pointer = read_u32(data, header + 20);
        sections.push_back(section);
    }

    // Data directory 6 is the debug directory; its entry is at optional+112 on
    // PE32+ and holds an RVA.
    size_t debug_directory = optional + 112 + 6 * 8;
    unsigned long debug_rva = read_u32(data, debug_directory);
    unsigned long debug_size = read_u32(data, debug_directory + 4);
    if (debug_rva == 0 || debug_size == 0) {
        error = image_path + L" has no debug directory";
        return false;
    }

    unsigned long debug_offset = 0;
    if (!rva_to_offset(sections, debug_rva, debug_offset)) {
        error = L"debug directory of " + image_path + L" is not backed by any section";
        return false;
    }

    for (unsigned long index = 0; index * 28 < debug_size; ++index) {
        size_t entry = debug_offset + static_cast<size_t>(index) * 28;
        if (entry + 28 > data.size()) {
            break;
        }
        unsigned long type = read_u32(data, entry + 12);
        unsigned long raw = read_u32(data, entry + 24);
        if (type != 2 || raw + 24 > data.size()) {  // IMAGE_DEBUG_TYPE_CODEVIEW
            continue;
        }
        if (::memcmp(data.data() + raw, "RSDS", 4) != 0) {
            continue;
        }

        unsigned char const* guid = data.data() + raw + 4;
        unsigned long age = read_u32(data, raw + 20);
        size_t name_start = raw + 24;
        size_t name_end = name_start;
        while (name_end < data.size() && data[name_end] != 0) {
            ++name_end;
        }
        if (name_end == name_start) {
            continue;
        }

        std::string ascii(reinterpret_cast<char const*>(data.data() + name_start), name_end - name_start);
        pdb_name.assign(ascii.begin(), ascii.end());
        identifier = format_identifier(guid, age);
        return true;
    }

    error = image_path + L" has no CodeView (RSDS) debug entry";
    return false;
}

std::wstring pdb_symbol_url(std::wstring const& pdb_name, std::wstring const& identifier) {
    return L"https://msdl.microsoft.com/download/symbols/" + pdb_name + L"/" + identifier + L"/" + pdb_name;
}

bool pdb_fetch(std::wstring const& pdb_name,
               std::wstring const& identifier,
               std::wstring const& cache_dir,
               std::wstring& pdb_path,
               std::wstring& error) {
    std::wstring store = join_path(join_path(cache_dir, pdb_name), identifier);
    if (!ensure_directory(cache_dir, error) || !ensure_directory(join_path(cache_dir, pdb_name), error) ||
        !ensure_directory(store, error)) {
        return false;
    }

    pdb_path = join_path(store, pdb_name);
    if (file_exists(pdb_path)) {
        return true;
    }

    std::wstring url = pdb_symbol_url(pdb_name, identifier);
    log_format(L"downloading %s", url.c_str());
    if (!http_download(url, pdb_path, error)) {
        return false;
    }

    LARGE_INTEGER size{};
    HANDLE file = ::CreateFileW(pdb_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        ::GetFileSizeEx(file, &size);
        ::CloseHandle(file);
    }
    log_format(L"downloaded %s (%lld bytes)", pdb_path.c_str(), size.QuadPart);
    return true;
}

}  // namespace deskfeed
