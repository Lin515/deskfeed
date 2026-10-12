#include "platform/taskbar_symbols.h"

#include "platform/log.h"
#include "platform/pdb.h"
#include "platform/symbols.h"
#include "platform/win32_util.h"

#include <windows.h>

#include <vector>

namespace deskfeed {
namespace {

constexpr wchar_t kSection[] = L"taskbar";

std::wstring taskbar_module_path() {
    wchar_t buffer[MAX_PATH]{};
    DWORD chars = ::GetEnvironmentVariableW(L"SystemRoot", buffer, ARRAYSIZE(buffer));
    std::wstring root = chars > 0 && chars < ARRAYSIZE(buffer) ? std::wstring(buffer, chars) : L"C:\\Windows";
    return root + L"\\System32\\taskbar.dll";
}

// %LOCALAPPDATA%\DeskFeed\symbols - the same store the standalone symbol tool
// fills, so a PDB fetched by either one is reused by the other.
std::wstring symbol_cache_dir() {
    return default_log_path(L"") + L"symbols";
}

std::wstring directory_of(std::wstring const& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring read_value(std::wstring const& path, wchar_t const* key) {
    wchar_t buffer[64]{};
    ::GetPrivateProfileStringW(kSection, key, L"", buffer, ARRAYSIZE(buffer), path.c_str());
    return buffer;
}

bool write_value(std::wstring const& path, wchar_t const* key, unsigned long long value) {
    wchar_t buffer[32]{};
    ::swprintf_s(buffer, L"0x%llX", value);
    return ::WritePrivateProfileStringW(kSection, key, buffer, path.c_str()) != FALSE;
}

}  // namespace

std::wstring taskbar_symbols_path() {
    return default_log_path(L"symbols.ini");
}

bool taskbar_symbols_resolve(TaskbarSymbols& symbols, std::wstring& error) {
    std::wstring module = taskbar_module_path();

    std::wstring pdb_name;
    std::wstring identifier;
    if (!pdb_identity_from_image(module, pdb_name, identifier, error)) {
        return false;
    }

    std::wstring pdb_path;
    if (!pdb_fetch(pdb_name, identifier, symbol_cache_dir(), pdb_path, error)) {
        return false;
    }
    log_format(L"taskbar.pdb %s -> %s", identifier.c_str(), pdb_path.c_str());

    // The directory holding the PDB is a valid plain DbgHelp symbol path, so no
    // symsrv.dll is involved.
    std::vector<SymbolRequest> requests = {
        {L"*GetTaskbarHost*", L"CTaskBand::GetTaskbarHost"},
        {L"*GetTaskbarHost*", L"CSecondaryTaskBand::GetTaskbarHost"},
        {L"*TaskbarHost*FrameHeight*", L"TaskbarHost::FrameHeight"},
        {L"*_Ref_count_base*Decref*", L"std::_Ref_count_base::_Decref"},
    };

    std::vector<SymbolMatch> matches;
    std::wstring resolve_error;
    bool resolved = symbol_resolve(module, L"", directory_of(pdb_path), requests, matches, resolve_error);

    if (!matches[0].resolved || !matches[1].resolved || !matches[2].resolved || !matches[3].resolved) {
        error = resolve_error.empty() ? L"the taskbar symbols are not all in the PDB" : resolve_error;
        return false;
    }

    symbols.get_taskbarhost = matches[0].rva;
    symbols.secondary_get_taskbarhost = matches[1].rva;
    symbols.taskbarhost_frame_height = matches[2].rva;
    symbols.ref_count_decref = matches[3].rva;

    log_format(L"symbols: CTaskBand::GetTaskbarHost=0x%llX CSecondaryTaskBand::GetTaskbarHost=0x%llX "
               L"TaskbarHost::FrameHeight=0x%llX _Ref_count_base::_Decref=0x%llX",
               symbols.get_taskbarhost, symbols.secondary_get_taskbarhost,
               symbols.taskbarhost_frame_height, symbols.ref_count_decref);

    (void)resolved;
    return true;
}

bool taskbar_symbols_store(TaskbarSymbols const& symbols, std::wstring& error) {
    std::wstring path = taskbar_symbols_path();
    if (!write_value(path, L"get_taskbarhost", symbols.get_taskbarhost) ||
        !write_value(path, L"secondary_get_taskbarhost", symbols.secondary_get_taskbarhost) ||
        !write_value(path, L"taskbarhost_frame_height", symbols.taskbarhost_frame_height) ||
        !write_value(path, L"ref_count_decref", symbols.ref_count_decref) ||
        !::WritePrivateProfileStringW(kSection, L"xaml", symbols.xaml_mode ? L"1" : L"0",
                                      path.c_str())) {
        error = L"cannot write " + path + L": " + last_error_text();
        return false;
    }
    // Flush the profile cache so the payload, which is a different process, sees
    // the values immediately.
    ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    log_format(L"stored taskbar symbols in %s (xaml=%d)", path.c_str(), symbols.xaml_mode ? 1 : 0);
    return true;
}

bool taskbar_xaml_requested() {
    return read_value(taskbar_symbols_path(), L"xaml") == L"1";
}

}  // namespace deskfeed
