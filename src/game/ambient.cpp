// ambient.cpp - see ambient.h.
//
// What the game does each frame (rage::tcManager, main thread): interpolate
// the weather keyframes into the current frame, then apply the script
// modifiers (primary, transition, extra) and the region/interior ones on
// top. A modifier is an array of (variable index, value, second value);
// vanilla files use the second value 1.0 for colour variables and 0.0 for
// intensities, and the plugin copies that convention. The extra slot is
// the one scripts use least, so the plugin's modifier lives there; if a
// server script takes it, the plugin backs off and says so in the menu.
#include "game/ambient.h"
#include "color/recolor.h"
#include "game/structs.h"
#include "hook/pattern.h"
#include "plugin/log.h"
#include "plugin/plugin.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vlights::ambient
{
	namespace
	{
		// --- layouts (Cfx gta-game-five/include/Timecycle.h) ----------------

		struct tcModData
		{
			int32_t index;
			float value1;
			float value2;
		};
		static_assert(sizeof(tcModData) == 12, "tcModData");

		struct tcModifier
		{
			rage::atArray<tcModData> modData; // +0
			uint32_t nameHash;                // +16
			uint32_t pad14;
			void* varMap;                     // +24  per-var lookup the game builds for file modifiers; null for created ones
			uint32_t userFlags;               // +32
			uint32_t unk;
		};
		static_assert(sizeof(tcModifier) == 0x28, "tcModifier");
		static_assert(offsetof(tcModifier, nameHash) == 16, "tcModifier::nameHash");
		static_assert(offsetof(tcModifier, varMap) == 24, "tcModifier::varMap");
		static_assert(offsetof(tcModifier, userFlags) == 32, "tcModifier::userFlags");

		struct tcVarInfo
		{
			int32_t index;
			int32_t pad4;
			const char* name; // +8
			float value;      // +16  default
			uint8_t end[12];
		};
		static_assert(sizeof(tcVarInfo) == 0x20, "tcVarInfo");

		struct DataPair // rage::atBinaryMap<tcModifier*>::DataPair
		{
			uint32_t hash;
			uint32_t pad4;
			tcModifier* value;
		};
		static_assert(sizeof(DataPair) == 16, "DataPair");

		struct tcManager
		{
			uint8_t pad0[8];
			rage::atArray<float> modifierStrengths;  // +8
			uint8_t pad18[0x28];
			rage::atArray<tcModifier*> modifiers;    // +0x40
			uint8_t mapSorted;                       // +0x50  atBinaryMap::m_isSorted
			uint8_t pad51[7];
			rage::atArray<DataPair> modifiersMap;    // +0x58
		};
		static_assert(offsetof(tcManager, modifiers) == 0x40, "tcManager::modifiers");
		static_assert(offsetof(tcManager, mapSorted) == 0x50, "tcManager::map");
		static_assert(offsetof(tcManager, modifiersMap) == 0x58, "tcManager::modifiersMap");

		// Cfx's TimecycleScriptData: "probably not even a struct, but seems
		// consistent between game builds".
		struct ScriptData
		{
			int32_t primaryIndex;       // (GET/SET)_TIMECYCLE_MODIFIER
			float primaryStrength;
			int32_t transitionIndex;
			float transitionStrength;
			float transitionSpeed;
			uint8_t pad14[24];
			int32_t extraIndex;         // +44  SET_EXTRA_TCMODIFIER / CLEAR_EXTRA_TCMODIFIER
		};
		static_assert(offsetof(ScriptData, extraIndex) == 44, "ScriptData::extraIndex");

		// --- Cfx's locators (Timecycle.cpp, sysAllocator.cpp; GTA_FIVE) ------
		//
		//   "7E 6E 45 33 F6": numVars = rel32 of the 6-byte instruction at
		//   match-6 (disp at +2); varInfos = rel32 of the 7-byte one at
		//   match+5 (disp at +3).
		//   "48 C1 E0 03 41 80 E1 01 C6 44 24 20 00" +19: 7-byte lea, disp at +3.
		//   "48 89 7B 10 8B 05 ? ? ? ? 83 F8 FF 74 13" +6: disp of the mov
		//   that reads m_primaryModifierIndex.
		//   "B9 ? ? ? ? 48 8B 0C 01 45 33 C9 49 8B D2 48" +1: the allocator's
		//   offset in the executable's TLS block.
		constexpr const char* kVarsPattern = "7E 6E 45 33 F6";
		constexpr const char* kManagerPattern = "48 C1 E0 03 41 80 E1 01 C6 44 24 20 00";
		constexpr const char* kScriptPattern = "48 89 7B 10 8B 05 ? ? ? ? 83 F8 FF 74 13";
		constexpr const char* kTlsPattern = "B9 ? ? ? ? 48 8B 0C 01 45 33 C9 49 8B D2 48";

		constexpr const char* kExtNames[6] = {
			"light_artificial_ext_down_col_r", "light_artificial_ext_down_col_g", "light_artificial_ext_down_col_b",
			"light_artificial_ext_up_col_r", "light_artificial_ext_up_col_g", "light_artificial_ext_up_col_b",
		};
		constexpr const char* kIntNames[6] = {
			"light_artificial_int_down_col_r", "light_artificial_int_down_col_g", "light_artificial_int_down_col_b",
			"light_artificial_int_up_col_r", "light_artificial_int_up_col_g", "light_artificial_int_up_col_b",
		};
		constexpr const char* kModifierName = "vlights_ambient";
		constexpr uint16_t kModCapacity = 12;
		constexpr float kColourSecondValue = 1.f; // what vanilla modifier files carry for *_col_* vars

		// --- state ---------------------------------------------------------

		// Our threads only; the game is never blocked on it. SRWLOCK rather than
		// std::mutex: the static mingw build has no pthread layer.
		SRWLOCK g_lock = SRWLOCK_INIT;
		struct Locked
		{
			Locked() { AcquireSRWLockExclusive(&g_lock); }
			~Locked() { ReleaseSRWLockExclusive(&g_lock); }
			Locked(const Locked&) = delete;
			Locked& operator=(const Locked&) = delete;
		};
		bool g_scanned = false;
		bool g_scanOk = false;
		bool g_validated = false; // manager contents checked (main thread, first map hook)
		bool g_available = false;
		std::string g_unavailableWhy;
		uintptr_t g_base = 0;
		size_t g_size = 0;

		tcManager* g_mgr = nullptr;
		ScriptData* g_script = nullptr;
		tcVarInfo** g_varInfos = nullptr;
		int* g_numVars = nullptr;
		uint32_t g_tlsOffset = 0;
		uint32_t* g_tlsIndex = nullptr;
		bool g_allocOk = false;

		int g_extIdx[6];
		int g_intIdx[6];
		bool g_intOk = false;

		uint32_t g_hash = 0;
		tcModifier* g_mod = nullptr;
		int g_modIndex = -1;
		bool g_registerPending = false;
		bool g_weSetSlot = false;
		bool g_slotTakenLogged = false;
		int g_validateTries = 0;
		uint64_t g_reclaims = 0;
		uint64_t g_reclaimWindowStart = 0; // GetTickCount64 of the current 10 s window
		unsigned g_reclaimsInWindow = 0;
		bool g_gaveUp = false;             // the game keeps clearing the slot: stop fighting it
		float g_lastExt[3] = { 0.f, 0.f, 0.f };

		// Static fallback when the game allocator cannot be reached: memory
		// the game may read forever and we never free.
		alignas(16) tcModifier g_staticMod;
		alignas(16) tcModData g_staticData[kModCapacity];

		// --- helpers -------------------------------------------------------

		uint32_t Joaat(const char* s)
		{
			uint32_t h = 0;
			for (; *s; ++s)
			{
				h += static_cast<uint8_t>(*s);
				h += h << 10;
				h ^= h >> 6;
			}
			h += h << 3;
			h ^= h >> 11;
			h += h << 15;
			return h;
		}

		bool ProbeReadable(const void* p, size_t n)
		{
			if (!p || n == 0)
				return false;
			uint8_t tmp;
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), p, &tmp, 1, &got) || got != 1)
				return false;
			if (n > 1 && (!ReadProcessMemory(GetCurrentProcess(), static_cast<const uint8_t*>(p) + n - 1, &tmp, 1, &got) || got != 1))
				return false;
			return true;
		}

		bool InImage(const void* p, size_t n = 1)
		{
			const uintptr_t a = reinterpret_cast<uintptr_t>(p);
			return a >= g_base && a + n <= g_base + g_size;
		}

		// Address encoded by a rip-relative instruction starting at `at`.
		uintptr_t Rel(uintptr_t at, int dispOffset, int length)
		{
			return at + length + *reinterpret_cast<const int32_t*>(at + dispOffset);
		}

		bool FindOne(const char* text, uintptr_t& out)
		{
			Pattern p;
			if (!ParsePattern(text, p))
			{
				Log("ambient: bad pattern text '%s'", text);
				return false;
			}
			auto hits = FindPattern(p, g_base, g_size, 2);
			if (hits.size() != 1)
			{
				Log("ambient: pattern '%s' matched %u times (need exactly 1); check Cfx Timecycle.cpp / sysAllocator.cpp for the current one", text, (unsigned)hits.size());
				return false;
			}
			out = hits[0];
			return true;
		}

		// Cfx hook::get_tls(): the executable's TLS block for this thread
		// (TEB->ThreadLocalStoragePointer[exe's TLS index]).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds" // mingw's __readgsqword header trips it
#endif
		char* ExeTls()
		{
			if (!g_tlsIndex)
				return nullptr;
			LPVOID* tlsBase = reinterpret_cast<LPVOID*>(__readgsqword(0x58));
			if (!tlsBase)
				return nullptr;
			return static_cast<char*>(tlsBase[*g_tlsIndex]);
		}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

		// rage::sysMemAllocator: XBR_VIRTUAL_BASE_2802(2); Allocate is base
		// slot 2 and Free base slot 4, both past the shift point.
		void* GameAllocator()
		{
			char* tls = ExeTls();
			if (!tls)
				return nullptr;
			return *reinterpret_cast<void**>(tls + g_tlsOffset);
		}

		void* GameAlloc(size_t n)
		{
			void* alloc = GameAllocator();
			if (!alloc)
				return nullptr;
			void** vt = *static_cast<void***>(alloc);
			using Fn = void* (*)(void*, size_t, size_t, int);
			Fn fn = reinterpret_cast<Fn>(vt[2 + StreamingVtableShift()]);
			void* p = fn(alloc, n, 16, 0);
			if (p)
				std::memset(p, 0, n);
			return p;
		}

		void GameFree(void* p)
		{
			void* alloc = GameAllocator();
			if (!alloc || !p)
				return;
			void** vt = *static_cast<void***>(alloc);
			using Fn = void (*)(void*, void*);
			Fn fn = reinterpret_cast<Fn>(vt[4 + StreamingVtableShift()]);
			fn(alloc, p);
		}

		// Grow a game atArray by `extra` entries the way the game does
		// (allocate, copy, free the old buffer with the game allocator).
		template <typename T>
		bool Grow(rage::atArray<T>& a, uint16_t extra)
		{
			if (!g_allocOk)
				return false;
			const uint32_t newCap = static_cast<uint32_t>(a.capacity) + extra;
			if (newCap > 0xFFFF)
				return false;
			T* buf = static_cast<T*>(GameAlloc(newCap * sizeof(T)));
			if (!buf)
				return false;
			if (a.data && a.count)
				std::memcpy(buf, a.data, a.count * sizeof(T));
			T* old = a.data;
			a.data = buf;
			a.capacity = static_cast<uint16_t>(newCap);
			if (old)
				GameFree(old);
			return true;
		}

		int VarIndex(const char* name)
		{
			tcVarInfo* infos = *g_varInfos;
			const int n = *g_numVars;
			for (int i = 0; i < n; ++i)
				if (infos[i].name && std::strcmp(infos[i].name, name) == 0)
					return infos[i].index;
			return -1;
		}

		// --- locate (DllMain or first map hook: pattern scans only) ---------

		bool Scan()
		{
			uintptr_t vars = 0, mgr = 0, script = 0, tls = 0;
			if (!FindOne(kVarsPattern, vars) || !FindOne(kManagerPattern, mgr) || !FindOne(kScriptPattern, script))
				return false;
			g_numVars = reinterpret_cast<int*>(Rel(vars - 6, 2, 6));
			g_varInfos = reinterpret_cast<tcVarInfo**>(Rel(vars + 5, 3, 7));
			g_mgr = reinterpret_cast<tcManager*>(Rel(mgr + 19, 3, 7));
			g_script = reinterpret_cast<ScriptData*>(Rel(script + 6, 0, 4));
			if (!InImage(g_numVars, 4) || !InImage(g_varInfos, 8) || !InImage(g_mgr, sizeof(tcManager)) || !InImage(g_script, sizeof(ScriptData)))
			{
				Log("ambient: a resolved static lies outside the game image (numVars=%p varInfos=%p manager=%p script=%p); not touching the timecycle",
					(void*)g_numVars, (void*)g_varInfos, (void*)g_mgr, (void*)g_script);
				return false;
			}

			// The allocator is optional: without it the modifier lives in
			// plugin memory and the list must have spare capacity.
			// The pattern is the instruction sequence the game uses to read the
			// allocator, which the build inlines more than once (two hits on
			// b3751); every hit carries the same immediate, so they must agree.
			g_allocOk = false;
			bool tlsFound = false;
			{
				Pattern p;
				std::vector<uintptr_t> hits;
				if (ParsePattern(kTlsPattern, p))
					hits = FindPattern(p, g_base, g_size, 8);
				if (!hits.empty())
				{
					const uint32_t first = *reinterpret_cast<const uint32_t*>(hits[0] + 1);
					tlsFound = std::all_of(hits.begin(), hits.end(), [&](uintptr_t h) { return *reinterpret_cast<const uint32_t*>(h + 1) == first; });
					if (tlsFound)
						tls = hits[0];
				}
				if (!tlsFound)
					Log("ambient: allocator tls pattern '%s' matched %u times with differing offsets; check Cfx sysAllocator.cpp", kTlsPattern, (unsigned)hits.size());
			}
			if (tlsFound)
			{
				g_tlsOffset = *reinterpret_cast<const uint32_t*>(tls + 1);
				auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
				auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
				const DWORD tlsRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress;
				if (tlsRva && g_tlsOffset < 0x10000)
				{
					auto dir = reinterpret_cast<const IMAGE_TLS_DIRECTORY*>(g_base + tlsRva);
					g_tlsIndex = reinterpret_cast<uint32_t*>(dir->AddressOfIndex);
					g_allocOk = g_tlsIndex != nullptr;
				}
			}
			Log("ambient: timecycle statics: numVars at %p, varInfos at %p, tcManager at %p (base+0x%llX), script data at %p; allocator tls offset 0x%X (%s)",
				(void*)g_numVars, (void*)g_varInfos, (void*)g_mgr, (unsigned long long)(reinterpret_cast<uintptr_t>(g_mgr) - g_base), (void*)g_script,
				g_tlsOffset, g_allocOk ? "ok" : "unavailable, plugin memory + spare capacity only");
			return true;
		}

		// --- validate + register (main thread, inside the map hook) ---------

		bool Fail(const char* why)
		{
			g_available = false;
			g_unavailableWhy = why;
			Log("ambient: unavailable: %s", why);
			return false;
		}

		bool Validate()
		{
			g_validated = true;
			const int n = *g_numVars;
			if (n < 50 || n > 4000)
				return Fail("implausible timecycle variable count");
			tcVarInfo* infos = *g_varInfos;
			if (!infos || !ProbeReadable(infos, sizeof(tcVarInfo) * static_cast<size_t>(n)))
				return Fail("variable table unreadable");
			for (int i = 0; i < n; ++i)
			{
				if (infos[i].index != i || !infos[i].name || !ProbeReadable(infos[i].name, 1))
					return Fail("variable table layout mismatch");
			}
			for (int i = 0; i < 6; ++i)
			{
				g_extIdx[i] = VarIndex(kExtNames[i]);
				g_intIdx[i] = VarIndex(kIntNames[i]);
			}
			for (int i = 0; i < 6; ++i)
				if (g_extIdx[i] < 0)
					return Fail("light_artificial_ext_* variables not found by name");
			g_intOk = std::all_of(std::begin(g_intIdx), std::end(g_intIdx), [](int i) { return i >= 0; });

			const tcManager* m = g_mgr;
			if ((!m->modifiers.data || m->modifiers.count == 0) && ++g_validateTries < 200)
			{
				g_validated = false; // timecycle not initialised yet: try again on a later block
				return false;
			}
			if (!m->modifiers.data || m->modifiers.count == 0 || m->modifiers.count > m->modifiers.capacity || m->modifiers.count > 4096)
				return Fail("modifier list implausible");
			if (!ProbeReadable(m->modifiers.data, sizeof(void*) * m->modifiers.count))
				return Fail("modifier list unreadable");
			if (!m->modifiersMap.data || m->modifiersMap.count > m->modifiersMap.capacity)
				return Fail("modifier map implausible");
			const ScriptData* s = g_script;
			if (s->primaryIndex < -1 || s->primaryIndex >= m->modifiers.count || s->extraIndex < -1 || s->extraIndex >= m->modifiers.count)
				return Fail("script data out of range");

			if (g_allocOk)
			{
				void* alloc = GameAllocator();
				void** vt = alloc ? *static_cast<void***>(alloc) : nullptr;
				const int slot = 2 + StreamingVtableShift();
				if (!alloc || !ProbeReadable(vt, sizeof(void*) * (slot + 3)) || !InImage(vt[slot]) || !InImage(vt[slot + 2]))
				{
					Log("ambient: game allocator not usable from here (allocator=%p); falling back to plugin memory", alloc);
					g_allocOk = false;
				}
			}

			Log("ambient: %d timecycle variables; ext colour vars at %d..%d / %d..%d, int %s; %u modifiers loaded (capacity %u, map %u, strengths %u); script slots: primary=%d (%.2f) transition=%d (%.2f, speed %.2f) extra=%d",
				n, g_extIdx[0], g_extIdx[2], g_extIdx[3], g_extIdx[5], g_intOk ? "found" : "missing",
				(unsigned)m->modifiers.count, (unsigned)m->modifiers.capacity, (unsigned)m->modifiersMap.count, (unsigned)m->modifierStrengths.count,
				s->primaryIndex, s->primaryStrength, s->transitionIndex, s->transitionStrength, s->transitionSpeed, s->extraIndex);
			{
				const uint32_t* after = reinterpret_cast<const uint32_t*>(s) + 12;
				Log("ambient: script data words after extra: %08X %08X %08X %08X", after[0], after[1], after[2], after[3]);
				// The manager's header, for finding the weather cycle data one day:
				// each qword, and for pointer-looking ones the first floats behind it.
				std::string t;
				const uint64_t* q = reinterpret_cast<const uint64_t*>(m);
				for (int i = 0; i < 32; ++i)
				{
					char b[160];
					const void* ptr = reinterpret_cast<const void*>(q[i]);
					if (q[i] > 0x10000 && q[i] < 0x7FFFFFFFFFFF && ProbeReadable(ptr, 16))
					{
						const float* f = static_cast<const float*>(ptr);
						snprintf(b, sizeof(b), " +%02X=%016llX->(%.3g,%.3g,%.3g,%.3g)", i * 8, (unsigned long long)q[i], f[0], f[1], f[2], f[3]);
					}
					else
					{
						snprintf(b, sizeof(b), " +%02X=%016llX", i * 8, (unsigned long long)q[i]);
					}
					t += b;
				}
				Log("ambient: tcManager header:%s", t.c_str());
			}

			// What loaded modifiers do with these variables: confirms the
			// (value, 1.0) convention on this install.
			int shown = 0;
			for (uint16_t i = 0; i < m->modifiers.count && shown < 6; ++i)
			{
				const tcModifier* mod = m->modifiers.data[i];
				if (!mod || !ProbeReadable(mod, sizeof(tcModifier)) || !mod->modData.data || mod->modData.count > 2048)
					continue;
				if (!ProbeReadable(mod->modData.data, sizeof(tcModData) * mod->modData.count))
					continue;
				for (uint16_t k = 0; k < mod->modData.count; ++k)
				{
					const tcModData& d = mod->modData.data[k];
					if (d.index == g_extIdx[0] || d.index == g_intIdx[0])
					{
						Log("ambient: loaded modifier #%u (hash %08X, %u vars) sets var %d to (%.3f, %.3f)", (unsigned)i, mod->nameHash, (unsigned)mod->modData.count, d.index, d.value1, d.value2);
						shown++;
						break;
					}
				}
			}
			g_available = true;
			return true;
		}

		// Write the entries for `cfg` into the modifier. Caller holds g_lock.
		void WriteEntries(tcModifier* mod, const Config& cfg)
		{
			float ext[3];
			AmbientColour(cfg.match.target, cfg.ambientBrightness, ext);
			tcModData entries[kModCapacity];
			uint16_t n = 0;
			for (int i = 0; i < 6; ++i)
				entries[n++] = tcModData{ g_extIdx[i], ext[i % 3], kColourSecondValue };
			if (cfg.ambientInterior && g_intOk)
				for (int i = 0; i < 6; ++i)
					entries[n++] = tcModData{ g_intIdx[i], ext[i % 3], kColourSecondValue };
			// Ascending variable index, as the game's files are ordered (insertion
			// sort: n <= 12, and it keeps GCC's bounds checker quiet).
			for (uint16_t i = 1; i < n; ++i)
				for (uint16_t j = i; j > 0 && entries[j - 1].index > entries[j].index; --j)
					std::swap(entries[j - 1], entries[j]);
			// Entries first, count last: a concurrent reader sees either the
			// old or the new set, every entry valid in both.
			for (uint16_t i = 0; i < n; ++i)
				mod->modData.data[i] = entries[i];
			mod->modData.count = n;
			std::memcpy(g_lastExt, ext, sizeof(ext));
		}

		// Claim or release the primary script slot (SET_TIMECYCLE_MODIFIER)
		// for `cfg`. The extra slot was tried first: the game clears it every
		// frame (a script has to re-set it each frame), so the plugin was
		// re-claiming it ten times a second, which showed as a constant
		// transition sound and no tint. The primary slot persists until a
		// script clears it. If something still clears it faster than a few
		// times per ten seconds, the plugin stops fighting and says so.
		// Caller holds g_lock.
		void SetSlot(const Config& cfg)
		{
			if (!g_mod || g_modIndex < 0 || g_gaveUp)
				return;
			ScriptData* s = g_script;
			const bool on = cfg.match.enabled && cfg.ambientEnabled;
			const int cur = s->primaryIndex;
			if (on)
			{
				if (cur == -1 || cur == g_modIndex)
				{
					if (cur != g_modIndex)
					{
						if (g_weSetSlot)
						{
							g_reclaims++;
							const uint64_t now = GetTickCount64();
							if (now - g_reclaimWindowStart > 10000)
							{
								g_reclaimWindowStart = now;
								g_reclaimsInWindow = 0;
							}
							if (++g_reclaimsInWindow > 5)
							{
								g_gaveUp = true;
								Log("ambient: the primary timecycle slot was cleared %u times in ten seconds; something re-sets it every frame. Giving up on the slot for this session (road glow stays as the game has it)", g_reclaimsInWindow);
								return;
							}
						}
						s->primaryIndex = g_modIndex;
						if (!(s->primaryStrength > 0.f))
							s->primaryStrength = 1.f;
					}
					g_weSetSlot = true;
					g_slotTakenLogged = false;
				}
				else if (!g_slotTakenLogged)
				{
					g_slotTakenLogged = true;
					Log("ambient: primary timecycle slot is held by modifier #%d (a script's); road glow tint paused until it is cleared", cur);
				}
			}
			else
			{
				if (cur == g_modIndex)
					s->primaryIndex = -1;
				g_weSetSlot = false;
			}
		}

		bool Register(const Config& cfg)
		{
			tcManager* m = g_mgr;
			if (!m->modifiers.data || m->modifiers.count > m->modifiers.capacity || m->modifiers.count > 4096)
				return Fail("modifier list implausible at registration");

			// A previous registration may have survived (the manager kept it).
			for (uint16_t i = 0; i < m->modifiers.count; ++i)
			{
				tcModifier* mod = m->modifiers.data[i];
				if (mod && ProbeReadable(mod, sizeof(tcModifier)) && mod->nameHash == g_hash && mod->modData.capacity == kModCapacity)
				{
					g_mod = mod;
					g_modIndex = i;
					LogDebug("ambient: modifier already present at #%d", g_modIndex);
					return true;
				}
			}

			tcModifier* mod = nullptr;
			tcModData* data = nullptr;
			if (g_allocOk)
			{
				mod = static_cast<tcModifier*>(GameAlloc(sizeof(tcModifier)));
				data = static_cast<tcModData*>(GameAlloc(sizeof(tcModData) * kModCapacity));
				if (!mod || !data)
					return Fail("game allocator returned null");
			}
			else
			{
				if (g_staticMod.modData.data) // used once already; the game may still read it
					return Fail("plugin-memory modifier already handed to the game once; restart to re-register");
				mod = &g_staticMod;
				data = g_staticData;
			}
			mod->modData.data = data;
			mod->modData.count = 0;
			mod->modData.capacity = kModCapacity;
			mod->nameHash = g_hash;
			mod->varMap = nullptr;
			mod->userFlags = 0;
			mod->unk = 0;
			WriteEntries(mod, cfg);

			const uint16_t oldCount = m->modifiers.count;
			if (m->modifiers.count >= m->modifiers.capacity && !Grow(m->modifiers, 16))
				return Fail("modifier list full and cannot grow (no game allocator)");
			if (m->modifierStrengths.data && m->modifierStrengths.count == oldCount)
			{
				// Parallel per-modifier array: keep it in step.
				if (m->modifierStrengths.count >= m->modifierStrengths.capacity && !Grow(m->modifierStrengths, 16))
					return Fail("strength list full and cannot grow");
				m->modifierStrengths.data[m->modifierStrengths.count] = 1.f;
				m->modifierStrengths.count++;
			}
			m->modifiers.data[oldCount] = mod;
			m->modifiers.count = oldCount + 1;
			g_mod = mod;
			g_modIndex = oldCount;

			// Name map (what lookups by name use): sorted insert if sorted.
			if (m->modifiersMap.data && (m->modifiersMap.count < m->modifiersMap.capacity || Grow(m->modifiersMap, 16)))
			{
				uint16_t pos = m->modifiersMap.count;
				if (m->mapSorted)
				{
					pos = 0;
					while (pos < m->modifiersMap.count && m->modifiersMap.data[pos].hash < g_hash)
						++pos;
					std::memmove(&m->modifiersMap.data[pos + 1], &m->modifiersMap.data[pos], sizeof(DataPair) * (m->modifiersMap.count - pos));
				}
				m->modifiersMap.data[pos] = DataPair{ g_hash, 0, mod };
				m->modifiersMap.count++;
			}
			else
			{
				Log("ambient: modifier name map not extended (full); lookups by name will not find '%s', which nothing needs", kModifierName);
			}

			Log("ambient: modifier '%s' (hash %08X) registered as #%d with %u vars (%s memory); ext colour (%.3f, %.3f, %.3f)",
				kModifierName, g_hash, g_modIndex, (unsigned)mod->modData.count, g_allocOk ? "game" : "plugin", g_lastExt[0], g_lastExt[1], g_lastExt[2]);
			return true;
		}

		// Worker thread: is our modifier still where we left it? Probes
		// every pointer; the list may have been reallocated or torn down.
		void FollowModifier()
		{
			if (!g_mod)
				return;
			const tcManager* m = g_mgr;
			tcModifier* const* data = m->modifiers.data;
			const uint16_t count = m->modifiers.count;
			if (data && count && count <= 4096 && ProbeReadable(data, sizeof(void*) * count))
			{
				if (g_modIndex >= 0 && g_modIndex < count && data[g_modIndex] == g_mod)
					return;
				for (uint16_t i = 0; i < count; ++i)
				{
					if (data[i] == g_mod)
					{
						LogDebug("ambient: modifier moved from #%d to #%u", g_modIndex, (unsigned)i);
						if (g_weSetSlot && g_script->primaryIndex == g_modIndex)
							g_script->primaryIndex = i;
						g_modIndex = i;
						return;
					}
				}
			}
			Log("ambient: modifier no longer in the timecycle manager's list; re-registering on the next map block");
			if (g_weSetSlot && g_script->primaryIndex == g_modIndex)
				g_script->primaryIndex = -1;
			g_weSetSlot = false;
			g_mod = nullptr;
			g_modIndex = -1;
			g_registerPending = true;
		}
	}

	void Install(uintptr_t imageBase, size_t imageSize)
	{
		Locked lk;
		if (g_scanned)
			return;
		g_scanned = true;
		g_base = imageBase;
		g_size = imageSize;
		g_hash = Joaat(kModifierName);
		g_scanOk = Scan();
		if (!g_scanOk)
			Fail("timecycle statics not located (patterns)");
	}

	void OnMapHook()
	{
		const ConfigPtr cfg = GetConfigPtr();
		if (!cfg->ambientEnabled)
			return;
		Locked lk;
		if (!g_scanned || !g_scanOk)
			return;
		if (!g_validated && !Validate())
			return;
		if (!g_available)
			return;
		if (!g_mod || g_registerPending)
		{
			g_registerPending = false;
			if (Register(*cfg))
				SetSlot(*cfg);
		}
	}

	void Apply(const Config& cfg)
	{
		Locked lk;
		if (!g_mod)
		{
			if (cfg.ambientEnabled && g_available)
				g_registerPending = true;
			return;
		}
		WriteEntries(g_mod, cfg);
		SetSlot(cfg);
	}

	void Tick()
	{
		Locked lk;
		if (!g_available || !g_mod)
			return;
		FollowModifier();
		if (!g_mod)
			return;
		const ConfigPtr cfg = GetConfigPtr();
		SetSlot(*cfg);
	}

	bool Available()
	{
		Locked lk;
		return g_available;
	}

	std::string Status()
	{
		Locked lk;
		char buf[256];
		if (!g_scanned)
			return "road glow tint: not installed";
		if (!g_available)
		{
			if (g_unavailableWhy.empty())
				return "road glow tint: waiting for the first map block";
			snprintf(buf, sizeof(buf), "road glow tint unavailable: %s", g_unavailableWhy.c_str());
			return buf;
		}
		if (!g_mod)
			return GetConfigPtr()->ambientEnabled ? "road glow tint: registers when the next map block loads" : "road glow tint: off";
		const int slot = g_script->primaryIndex;
		const char* state = g_gaveUp ? "given up: the slot is cleared every frame" : slot == g_modIndex ? "active" : (slot == -1 ? "off" : "paused: a script holds the primary timecycle slot");
		snprintf(buf, sizeof(buf), "road glow tint: modifier #%d, %u vars, colour (%.2f, %.2f, %.2f), %s; slot reclaimed %llu times",
			g_modIndex, g_mod ? (unsigned)g_mod->modData.count : 0u, g_lastExt[0], g_lastExt[1], g_lastExt[2], state, (unsigned long long)g_reclaims);
		return buf;
	}
}
