#pragma once

#include <string>

// The DeskFeed host's symbol file: the addresses of taskbar.dll internals, plus
// the mode the injector was asked for.
//
// They are resolved by the injector, which is a console process where loading
// DbgHelp and fetching a PDB is harmless, and stored in a small file. The payload
// runs inside explorer.exe; it should not load a symbol engine there, and it
// should not download anything.
//
// The payload no longer needs the addresses — reaching the taskbar's XAML tree
// goes through the public diagnostics interfaces (see payload/xaml_tap.h) — but
// the addresses remain the tool's output and the record of what the private
// route would take. Resolving them is therefore allowed to fail: an injection
// that only needs the XAML route must not depend on a symbol server.
namespace deskfeed {

struct TaskbarSymbols {
    unsigned long long get_taskbarhost = 0;
    unsigned long long secondary_get_taskbarhost = 0;
    unsigned long long taskbarhost_frame_height = 0;
    unsigned long long ref_count_decref = 0;
    // The injector's --xaml switch. The payload reads it to decide between
    // inserting a block into the taskbar's XAML tree and drawing the diagnostic
    // probe row; the probe row would sit exactly where the tray block goes.
    bool xaml_mode = false;
};

// %LOCALAPPDATA%\DeskFeed\symbols.ini
std::wstring taskbar_symbols_path();

// The mode flag alone, for the payload's startup decision. False when no symbol
// file exists yet.
bool taskbar_xaml_requested();

// DbgHelp side (injector): identifies the PDB of taskbar.dll, fetches it into the
// symbol cache and resolves the four addresses.
bool taskbar_symbols_resolve(TaskbarSymbols& symbols, std::wstring& error);

bool taskbar_symbols_store(TaskbarSymbols const& symbols, std::wstring& error);

}  // namespace deskfeed
