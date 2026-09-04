#pragma once
#include <string>

namespace vlights
{
	// Truncates and opens the log file once; it stays open (one flush per
	// line) so logging never pays for an open/close, which the antivirus
	// scans, on the game's threads.
	void LogInit(const std::wstring& path);

	// printf-style, thread-safe, one line per call with a timestamp. Use for
	// startup, errors and one-off findings only.
	void Log(const char* fmt, ...);

	// Same, but dropped entirely unless SetDebugLogging(true). Use for anything
	// that fires per model, per map block or per frame.
	void LogDebug(const char* fmt, ...);
	void SetDebugLogging(bool on);
	bool DebugLogging();
}
