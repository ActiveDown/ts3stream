#pragma once

#include <string>

namespace ts3stream::diag {

// Initializes the diagnostic log in %APPDATA%\TS3Client\plugins\ts3stream\diag.log.
void initLog();

// Writes a formatted line to the diagnostic log and OutputDebugString.
void log(const char* fmt, ...);

// Returns the directory used for diagnostics/config.
std::wstring dataDir();

// Returns the directory containing this plugin DLL.
std::wstring moduleDir();

std::string toUtf8(const std::wstring& value);

} // namespace ts3stream::diag
