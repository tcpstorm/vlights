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
