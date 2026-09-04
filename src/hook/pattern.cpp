#include "hook/pattern.h"

#include <windows.h>
#include <algorithm>
#include <sstream>

namespace lodlight
{
	bool ParsePattern(const std::string& text, Pattern& out)
	{
		out.bytes.clear();
		out.mask.clear();

		std::istringstream ss(text);
		std::string tok;
		while (ss >> tok)
		{
			if (tok == "?" || tok == "??")
			{
				out.bytes.push_back(0);
				out.mask.push_back(false);
				continue;
			}
			if (tok.size() != 2)
				return false;
			char* end = nullptr;
			unsigned long v = strtoul(tok.c_str(), &end, 16);
			if (!end || *end != '\0' || v > 0xFF)
				return false;
			out.bytes.push_back(static_cast<uint8_t>(v));
			out.mask.push_back(true);
		}
		return !out.bytes.empty();
	}

	int GameBuild()
	{
		static int cached = -1;
		if (cached >= 0)
			return cached;
		cached = 0;
		wchar_t path[MAX_PATH];
		DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
		for (DWORD i = 0; i + 2 < n; ++i)
		{
			if (path[i] == L'_' && path[i + 1] == L'b' && path[i + 2] >= L'0' && path[i + 2] <= L'9')
			{
				int v = 0;
				for (DWORD j = i + 2; j < n && path[j] >= L'0' && path[j] <= L'9'; ++j)
					v = v * 10 + (path[j] - L'0');
				cached = v;
				break;
			}
		}
		return cached;
	}

	int StreamingVtableShift()
	{
		const int b = GameBuild();
		return (b == 0 || b >= 2802) ? 6 : 0;
	}

	bool GetMainModuleRange(uintptr_t& base, size_t& size)
	{
		HMODULE mod = GetModuleHandleW(nullptr);
		if (!mod)
			return false;

		auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(mod) + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;

		base = reinterpret_cast<uintptr_t>(mod);
		size = nt->OptionalHeader.SizeOfImage;
		return size != 0;
	}

	static bool MatchAt(const uint8_t* p, const Pattern& pat)
	{
		for (size_t i = 0; i < pat.bytes.size(); ++i)
		{
			if (pat.mask[i] && p[i] != pat.bytes[i])
				return false;
		}
		return true;
	}

	std::vector<uintptr_t> FindPattern(const Pattern& pat, uintptr_t start, size_t size, size_t maxResults)
	{
		std::vector<uintptr_t> hits;
		const uintptr_t end = start + size;
		const size_t n = pat.bytes.size();

		uintptr_t cur = start;
		while (cur < end && hits.size() < maxResults)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0)
				break;

			uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
			uintptr_t regionEnd = regionStart + mbi.RegionSize;
			uintptr_t scanFrom = std::max(cur, regionStart);
			uintptr_t scanTo = std::min(end, regionEnd);

			const bool readable = mbi.State == MEM_COMMIT
				&& !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
				&& (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));

			if (readable && scanTo > scanFrom && (scanTo - scanFrom) >= n)
			{
				const uint8_t* p = reinterpret_cast<const uint8_t*>(scanFrom);
				const size_t len = scanTo - scanFrom;
				for (size_t i = 0; i + n <= len; ++i)
				{
					if (p[i] == pat.bytes[0] || !pat.mask[0])
					{
						if (MatchAt(p + i, pat))
						{
							hits.push_back(scanFrom + i);
							if (hits.size() >= maxResults)
								break;
						}
					}
				}
			}

			cur = regionEnd;
		}
		return hits;
	}
}
