#include "platform/symbols.h"

#include "platform/log.h"
#include "platform/win32_util.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstring>
#include <mutex>

namespace deskfeed {
namespace {

// The address DbgHelp reports is only used to derive an RVA, so a fixed base is
// enough: the caller never needs the module's real load address.
constexpr unsigned long long kSymbolBase = 0x10000000;

using SymInitializeW_t = BOOL(WINAPI*)(HANDLE, PCWSTR, BOOL);
using SymSetOptions_t = DWORD(WINAPI*)(DWORD);
using SymLoadModuleExW_t = DWORD64(WINAPI*)(HANDLE, HANDLE, PCWSTR, PCWSTR, DWORD64, DWORD,
                                            PMODLOAD_DATA, DWORD);
// SymEnumSymbols takes the SYMBOL_INFO-style callback (PSYM_ENUMERATESYMBOLS_*),
// not the older name/address one (PSYM_ENUMSYMBOLS_*).
using SymEnumSymbolsW_t = BOOL(WINAPI*)(HANDLE, ULONG64, PCWSTR, PSYM_ENUMERATESYMBOLS_CALLBACKW, PVOID);
using SymGetModuleInfoW64_t = BOOL(WINAPI*)(HANDLE, DWORD64, PIMAGEHLP_MODULEW64);
using SymFromAddrW_t = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFOW);
using SymCleanup_t = BOOL(WINAPI*)(HANDLE);

struct Engine {
    HMODULE module = nullptr;
    SymInitializeW_t initialize = nullptr;
    SymSetOptions_t set_options = nullptr;
    SymLoadModuleExW_t load_module = nullptr;
    SymEnumSymbolsW_t enum_symbols = nullptr;
    SymGetModuleInfoW64_t module_info = nullptr;
    SymFromAddrW_t from_addr = nullptr;
    SymCleanup_t cleanup = nullptr;
};

// DbgHelp loaded once per process. The library keeps global state, and loading a
// second copy for every lookup would be both wasteful and confusing.
std::once_flag g_engine_once;
Engine g_engine;
std::wstring g_engine_error;

bool load_engine(std::wstring const& dbghelp_dir, std::wstring& error) {
    std::wstring path = dbghelp_dir.empty() ? L"dbghelp.dll" : join_path(dbghelp_dir, L"dbghelp.dll");
    HMODULE module = ::LoadLibraryW(path.c_str());
    if (module == nullptr) {
        error = L"cannot load " + path + L": " + last_error_text();
        return false;
    }

    struct Entry {
        char const* name;
        void** target;
    };
    Entry entries[] = {
        {"SymInitializeW", reinterpret_cast<void**>(&g_engine.initialize)},
        {"SymSetOptions", reinterpret_cast<void**>(&g_engine.set_options)},
        {"SymLoadModuleExW", reinterpret_cast<void**>(&g_engine.load_module)},
        {"SymEnumSymbolsW", reinterpret_cast<void**>(&g_engine.enum_symbols)},
        {"SymGetModuleInfoW64", reinterpret_cast<void**>(&g_engine.module_info)},
        {"SymFromAddrW", reinterpret_cast<void**>(&g_engine.from_addr)},
        {"SymCleanup", reinterpret_cast<void**>(&g_engine.cleanup)},
    };

    for (Entry const& entry : entries) {
        *entry.target = reinterpret_cast<void*>(::GetProcAddress(module, entry.name));
        if (*entry.target == nullptr) {
            error = path + L" has no " + std::wstring(entry.name, entry.name + ::strlen(entry.name));
            return false;
        }
    }

    g_engine.module = module;
    return true;
}

struct Lookup {
    SymbolRequest const* request = nullptr;
    SymbolMatch* match = nullptr;
};

BOOL CALLBACK enum_callback(PSYMBOL_INFOW info, ULONG, PVOID raw) {
    Lookup& lookup = *static_cast<Lookup*>(raw);
    if (info == nullptr || info->Name == nullptr) {
        return TRUE;
    }

    std::wstring name(info->Name, info->NameLen);
    if (name.find(lookup.request->key) == std::wstring::npos) {
        return TRUE;
    }

    // Several enumerated names can contain the key (a decorated form, a base
    // class copy of a vftable, ...). The shortest one is the public name the
    // caller asked for.
    if (!lookup.match->resolved || name.size() < lookup.match->name.size()) {
        lookup.match->name = name;
        lookup.match->rva = info->Address - info->ModBase;
        lookup.match->resolved = true;
    }
    return TRUE;
}

struct Dump {
    std::vector<std::wstring>* names = nullptr;
    size_t limit = 0;
};

BOOL CALLBACK dump_callback(PSYMBOL_INFOW info, ULONG, PVOID raw) {
    Dump& dump = *static_cast<Dump*>(raw);
    if (info == nullptr || info->Name == nullptr) {
        return TRUE;
    }
    if (dump.names->size() >= dump.limit) {
        return FALSE;  // stop the enumeration once the diagnostic has enough
    }
    dump.names->emplace_back(info->Name, info->NameLen);
    return TRUE;
}

}  // namespace

bool symbol_resolve(std::wstring const& module_path,
                    std::wstring const& dbghelp_dir,
                    std::wstring const& search_path,
                    std::vector<SymbolRequest> const& requests,
                    std::vector<SymbolMatch>& results,
                    std::wstring& error) {
    results.assign(requests.size(), SymbolMatch{});

    std::call_once(g_engine_once, [&] {
        if (!load_engine(dbghelp_dir, g_engine_error)) {
            return;
        }
        // SYMOPT_UNDNAME keeps names readable; critical errors are reported
        // instead of opening a dialog inside explorer.exe.
        g_engine.set_options(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    });
    if (g_engine.module == nullptr) {
        error = g_engine_error;
        return false;
    }

    HANDLE process = ::GetCurrentProcess();
    std::wstring path = search_path;
    if (path.empty()) {
        error = L"a symbol search path is required";
        return false;
    }

    if (!g_engine.initialize(process, path.c_str(), FALSE)) {
        error = L"SymInitializeW failed: " + last_error_text();
        return false;
    }

    DWORD64 base = g_engine.load_module(process, nullptr, module_path.c_str(), nullptr, kSymbolBase, 0,
                                        nullptr, 0);
    if (base == 0) {
        error = L"SymLoadModuleExW failed for " + module_path + L": " + last_error_text();
        g_engine.cleanup(process);
        return false;
    }

    IMAGEHLP_MODULEW64 info{};
    info.SizeOfStruct = sizeof(info);
    unsigned long symbol_type = 0;
    if (g_engine.module_info(process, base, &info)) {
        symbol_type = info.SymType;
    }

    bool all_resolved = true;
    for (size_t index = 0; index < requests.size(); ++index) {
        Lookup lookup;
        lookup.request = &requests[index];
        lookup.match = &results[index];
        if (!g_engine.enum_symbols(process, base, requests[index].mask.c_str(), enum_callback, &lookup)) {
            log_format(L"symbol enumeration for %s failed: %s", requests[index].mask.c_str(),
                       last_error_text().c_str());
        }
        if (!results[index].resolved) {
            all_resolved = false;
        }
    }

    if (!all_resolved) {
        error = L"some symbols were not resolved (module SymType=" + std::to_wstring(symbol_type) +
                L", 3 means PDB; 0 means no symbols were loaded at all)";
    }

    g_engine.cleanup(process);
    return all_resolved;
}

bool symbol_enumerate(std::wstring const& module_path,
                      std::wstring const& dbghelp_dir,
                      std::wstring const& search_path,
                      std::wstring const& mask,
                      size_t limit,
                      std::vector<std::wstring>& names,
                      std::wstring& error) {
    std::call_once(g_engine_once, [&] {
        if (!load_engine(dbghelp_dir, g_engine_error)) {
            return;
        }
        g_engine.set_options(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    });
    if (g_engine.module == nullptr) {
        error = g_engine_error;
        return false;
    }

    HANDLE process = ::GetCurrentProcess();
    if (!g_engine.initialize(process, search_path.c_str(), FALSE)) {
        error = L"SymInitializeW failed: " + last_error_text();
        return false;
    }

    DWORD64 base = g_engine.load_module(process, nullptr, module_path.c_str(), nullptr, kSymbolBase, 0,
                                        nullptr, 0);
    if (base == 0) {
        error = L"SymLoadModuleExW failed for " + module_path + L": " + last_error_text();
        g_engine.cleanup(process);
        return false;
    }

    Dump dump;
    dump.names = &names;
    dump.limit = limit;
    bool ok = g_engine.enum_symbols(process, base, mask.c_str(), dump_callback, &dump) != FALSE;
    if (!ok && names.empty()) {
        error = L"symbol enumeration failed: " + last_error_text();
    }

    g_engine.cleanup(process);
    return ok || !names.empty();
}

bool symbol_at_rva(std::wstring const& module_path,
                   std::wstring const& dbghelp_dir,
                   std::wstring const& search_path,
                   unsigned long long rva,
                   std::wstring& name,
                   unsigned long long& displacement,
                   std::wstring& error) {
    std::call_once(g_engine_once, [&] {
        if (!load_engine(dbghelp_dir, g_engine_error)) {
            return;
        }
        g_engine.set_options(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    });
    if (g_engine.module == nullptr) {
        error = g_engine_error;
        return false;
    }

    HANDLE process = ::GetCurrentProcess();
    if (!g_engine.initialize(process, search_path.c_str(), FALSE)) {
        error = L"SymInitializeW failed: " + last_error_text();
        return false;
    }

    DWORD64 base = g_engine.load_module(process, nullptr, module_path.c_str(), nullptr, kSymbolBase, 0,
                                        nullptr, 0);
    if (base == 0) {
        error = L"SymLoadModuleExW failed for " + module_path + L": " + last_error_text();
        g_engine.cleanup(process);
        return false;
    }

    // SYMBOL_INFOW ends in a flexible name array, so the buffer is allocated by
    // hand rather than declared as the struct.
    constexpr size_t kNameChars = 512;
    std::vector<unsigned char> storage(sizeof(SYMBOL_INFOW) + kNameChars * sizeof(wchar_t));
    auto* info = reinterpret_cast<SYMBOL_INFOW*>(storage.data());
    info->SizeOfStruct = sizeof(SYMBOL_INFOW);
    info->MaxNameLen = static_cast<ULONG>(kNameChars);

    DWORD64 symbol_displacement = 0;
    bool ok = g_engine.from_addr(process, base + rva, &symbol_displacement, info) != FALSE;
    if (ok) {
        name.assign(info->Name, info->NameLen);
        displacement = symbol_displacement;
    } else {
        error = L"no symbol covers " + module_path + L"+0x" + hex_text(rva);
    }

    g_engine.cleanup(process);
    return ok;
}

}  // namespace deskfeed
