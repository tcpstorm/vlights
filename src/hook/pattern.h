#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vlights
{
	struct Pattern
	{
		std::vector<uint8_t> bytes;
		std::vector<bool> mask; // true = byte must match, false = wildcard
	};

	// "25 00 0C ? ? 49 8B 06" style. Returns false on a malformed token.
	bool ParsePattern(const std::string& text, Pattern& out);

	// Base and SizeOfImage of the main executable module (the game image).
	bool GetMainModuleRange(uintptr_t& base, size_t& size);

	// Game build parsed from the main module name (FiveM_b3751_GTAProcess.exe),
	// 0 if it cannot be read.
	int GameBuild();

	// strStreamingModule vtable layout: on builds >= 2802 every virtual sits
	// six slots later than on older builds (Cfx XBRVirtual.h,
	// XBR_VIRTUAL_BASE_2802(0) => Offset 6). Base slots per Cfx Streaming.h:
	// 3 Remove (unload), 4 RemoveSlot, 5 Load, 6 PlaceResource, 7 SetResource,
	// 8 GetPtr. So on b3751: Remove = 9, SetResource = 13, GetPtr = 14.
	int StreamingVtableShift();

	// Scans committed, readable pages in [start, start+size). Stops after
	// maxResults hits so uniqueness can be checked cheaply.
	std::vector<uintptr_t> FindPattern(const Pattern& p, uintptr_t start, size_t size, size_t maxResults);
}
