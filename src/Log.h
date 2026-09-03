#pragma once
#include <string>

namespace lodlight
{
	// Truncates and opens the log file. Safe to call once at startup.
	void LogInit(const std::wstring& path);
	// printf-style, thread-safe, one line per call with a timestamp.
	void Log(const char* fmt, ...);
}
