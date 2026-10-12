#pragma once

#include <string>

// Logging shared by the injector and the injected payload.
//
// The injector logs to the console, the payload logs to a file from inside
// explorer.exe. Every line is flushed as it is written so the file can be read
// while the target process is still running.
namespace deskfeed {

// Opens (truncating) the log file. When mirror_to_console is set the same lines
// are also written to the standard error stream.
bool log_open(std::wstring const& path, bool mirror_to_console);

void log_close();

// True once log_open has succeeded.
bool log_is_open();

void log_line(std::wstring const& text);

void log_format(wchar_t const* format, ...);

// %LOCALAPPDATA%\DeskFeed\<file_name>, falling back to the directory that
// holds the running module when LOCALAPPDATA is not set.
std::wstring default_log_path(wchar_t const* file_name);

}  // namespace deskfeed
