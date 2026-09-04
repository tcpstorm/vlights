#include "plugin/log.h"

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace lodlight
{
	namespace
	{
		SRWLOCK g_lock = SRWLOCK_INIT;
		FILE* g_file = nullptr;
		std::atomic<bool> g_debug{ false };

		void Write(const char* fmt, va_list ap)
		{
			char msg[2048];
			vsnprintf(msg, sizeof(msg), fmt, ap);

			SYSTEMTIME t;
			GetLocalTime(&t);

			AcquireSRWLockExclusive(&g_lock);
			if (g_file)
			{
				fprintf(g_file, "[%02u:%02u:%02u.%03u] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
				fflush(g_file);
			}
			ReleaseSRWLockExclusive(&g_lock);
		}
	}

	void LogInit(const std::wstring& path)
	{
		AcquireSRWLockExclusive(&g_lock);
		if (g_file)
			fclose(g_file);
		g_file = _wfopen(path.c_str(), L"w");
		ReleaseSRWLockExclusive(&g_lock);
	}

	void Log(const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		Write(fmt, ap);
		va_end(ap);
	}

	void LogDebug(const char* fmt, ...)
	{
		if (!g_debug.load(std::memory_order_relaxed))
			return;
		va_list ap;
		va_start(ap, fmt);
		Write(fmt, ap);
		va_end(ap);
	}

	void SetDebugLogging(bool on)
	{
		g_debug = on;
	}

	bool DebugLogging()
	{
		return g_debug.load(std::memory_order_relaxed);
	}
}
