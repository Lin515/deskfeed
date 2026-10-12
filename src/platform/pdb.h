#pragma once

#include <string>

// CodeView (RSDS) identity of a PE image, plus the Microsoft public symbol
// server fetch that turns it into a local PDB.
//
// Windows system DLLs ship without PDBs, and the parts of taskbar.dll this
// project needs are not exported, so the names have to come from the matching
// PDB. Fetching it here keeps the symbol half self-contained: no symsrv.dll
// beside dbghelp.dll is required, because DbgHelp can be pointed at a plain
// directory that already holds the downloaded file.
namespace deskfeed {

// Reads the CodeView debug directory of |image_path| and returns the PDB file
// name and the symbol server identifier ("<GUID><age>", uppercase, no dashes).
bool pdb_identity_from_image(std::wstring const& image_path,
                             std::wstring& pdb_name,
                             std::wstring& identifier,
                             std::wstring& error);

// Download URL of that PDB on Microsoft's public symbol server.
std::wstring pdb_symbol_url(std::wstring const& pdb_name, std::wstring const& identifier);

// Downloads the PDB into |cache_dir| unless it is already there, using the
// symbol server's own store layout:
//   <cache_dir>\<pdb_name>\<identifier>\<pdb_name>
// The returned path is the file itself. The directory holding it is also a valid
// plain DbgHelp symbol path, because the file inside keeps the plain PDB name.
bool pdb_fetch(std::wstring const& pdb_name,
               std::wstring const& identifier,
               std::wstring const& cache_dir,
               std::wstring& pdb_path,
               std::wstring& error);

}  // namespace deskfeed
