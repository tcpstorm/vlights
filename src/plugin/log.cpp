#include "plugin/log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace lodlight
{
	static SRWLOCK g_lock = SRWLOCK_INIT;
	static std::wstring g_path;

	void LogInit(const std::wstring& path)
	{
		AcquireSRWLockExclusive(&g_lock);
		g_path = path;
		if (FILE* f = _wfopen(g_path.c_str(), L"w"))
			fclose(f);
		ReleaseSRWLockExclusive(&g_lock);
	}

	void Log(const char* fmt, ...)
	{
		char msg[2048];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(msg, sizeof(msg), fmt, ap);
		va_end(ap);

		SYSTEMTIME t;
		GetLocalTime(&t);

		AcquireSRWLockExclusive(&g_lock);
		if (!g_path.empty())
		{
			if (FILE* f = _wfopen(g_path.c_str(), L"a"))
			{
				fprintf(f, "[%02u:%02u:%02u.%03u] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
				fclose(f);
			}
		}
		ReleaseSRWLockExclusive(&g_lock);
	}
}
