#pragma once

#include <string>
#include <vector>

// Minimal symbol lookup on top of DbgHelp.
//
// It exists so that the DeskFeed host can turn internal symbol names of
// taskbar.dll into addresses without a hooking framework: reaching the taskbar's
// XAML root needs three addresses, not hooks. What the caller gets back is an
// RVA, so it is independent of where the module happens to be loaded.
namespace deskfeed {

struct SymbolRequest {
    // DbgHelp enumeration mask, e.g. L"*GetTaskbarHost*".
    std::wstring mask;
    // Substring the enumerated name must contain to be accepted.
    std::wstring key;
};

struct SymbolMatch {
    std::wstring name;           // name as DbgHelp reported it
    unsigned long long rva = 0;  // symbol address - module base
    bool resolved = false;
};

// Resolves |requests| against |module_path|. |search_path| is a DbgHelp symbol
// path; a plain directory that holds the PDB is enough, and so is
// "srv*<cache>*https://msdl.microsoft.com/download/symbols", which additionally
// needs symsrv.dll next to the dbghelp.dll that is used. |dbghelp_dir| selects
// that dbghelp.dll (empty = the one the system resolves); pass the directory of
// the calling module to prefer a copy that sits next to it.
bool symbol_resolve(std::wstring const& module_path,
                    std::wstring const& dbghelp_dir,
                    std::wstring const& search_path,
                    std::vector<SymbolRequest> const& requests,
                    std::vector<SymbolMatch>& results,
                    std::wstring& error);

// Diagnostic: appends up to |limit| names matching |mask| to |names|. It answers
// "is this name in the public PDB at all", which is how a vftable that only
// exists in the private symbol stream is told apart from a wrong mask.
bool symbol_enumerate(std::wstring const& module_path,
                      std::wstring const& dbghelp_dir,
                      std::wstring const& search_path,
                      std::wstring const& mask,
                      size_t limit,
                      std::vector<std::wstring>& names,
                      std::wstring& error);

// Diagnostic: the symbol that contains |rva|, plus the offset into it. Used to
// name the function a fault address landed in.
bool symbol_at_rva(std::wstring const& module_path,
                   std::wstring const& dbghelp_dir,
                   std::wstring const& search_path,
                   unsigned long long rva,
                   std::wstring& name,
                   unsigned long long& displacement,
                   std::wstring& error);

}  // namespace deskfeed
