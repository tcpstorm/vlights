// near_lights.cpp - near-tier light recolouring: lights baked into models,
// and light-effect extensions on archetypes.
//
// Nearby lamps are lit by CLightAttr entries baked into the prop's drawable
// (or fragment, since most lamp posts are breakable .yft), or by a
// CExtensionDefLightEffect on the archetype in its .ytyp. Neither comes
// from the LOD ymaps. Both are intercepted when they finish loading and
// rewritten with the same matcher and target as the LOD tier.
//
// Everything below was verified live on b3751 and is checked at runtime
// against the ymap store the map hook already receives:
//   - streaming::Manager: Cfx code/components/gta-streaming-five/src/
//     Streaming.cpp, get_address(get_pattern("74 1A 8B 15 ? ? ? ? 48 8D 0D
//     ? ? ? ? 41", 11)); every match decodes to the same global.
//   - The module table is found by searching the manager for the known
//     ymap store (24 modules, inline at manager+0x1C0).
//   - Modules carry their RAGE class name at +24 ("MapDataStore",
//     "DrawableStore", "FragmentStore", "DwdStore"). No extension string.
//   - The streaming engine completes a model load with `call [vtable+0x68]`
//     (slot 13; args: store, slot index). On the model stores that is one
//     shared 22-byte stub forwarding to slot 43; it is detoured by code with
//     a thunk that passes all registers through and only acts for the three
//     stores of interest.
//   - The placed model is the first field of the store's pool entry
//     (atPoolBase at store+56; name hash at +12). NEVER call vtable slot 8
//     for this: on these stores it writes through a null array.
//   - Model layouts (verified live: names read back, lights decode):
//       gtaDrawable  : name ptr +0xA8, lights ptr +0xB0, count u16 +0xB8
//       gtaFragType  : name ptr +0x58, lights +0x110/+0x118, drawable +0x30,
//                      drawable array +0x38 / count u32 +0x48
//       pgDictionary<gtaDrawable>: drawable* array +0x30, count u16 +0x38
//       CLightAttr   : 168 bytes; colour +24..26, flashiness +27,
//                      intensity +28, type +38, volume colour +84..86
//   - Archetype lights: CMapTypes archetypes atArray +24 (Cfx),
//     fwArchetypeDef extensions atArray +120 (CodeWalker CBaseArchetypeDef),
//     fwExtensionDef type getter = vtable slot 7 on b2802+ (six filler
//     virtuals before the destructor, per Cfx's fwExtensionDefImpl2802),
//     slot 1 before; chosen at runtime. parStructure m_nameHash +8 is the
//     case-sensitive joaat of the class name: CExtensionDefLightEffect =
//     0x27922C43. Extension: instances atArray +32 of 160-byte
//     CLightAttrDef with colour +20, flashiness +23, volume colour +80.
//     Archetype files load before the first map block, so their hook is
//     installed from DllMain: the archetype store's FinishLoading is the
//     sibling of the ymap one, found from the shared call-site bytes.
#include "game/near_lights.h"
#include "game/textures.h"
#include "color/recolor.h"
#include "game/track.h"
#include "hook/pattern.h"
#include "plugin/log.h"
#include "plugin/plugin.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vlights::nearlights
{
	namespace
	{
		constexpr const char* kManagerPattern = "74 1A 8B 15 ? ? ? ? 48 8D 0D ? ? ? ? 41";
		constexpr ptrdiff_t kManagerOffset = 11;
		constexpr size_t kModuleMgrOffset = 144;   // static_assert in Cfx Streaming.h
		constexpr size_t kManagerScanSpan = 0x1000;
		constexpr size_t kStoreNameOffsetMax = 64;
		constexpr size_t kPoolOffset = 56;
		// strStreamingModule::SetResource (base slot 7 + the 2802 shift): the
		// call that publishes a finished load. See hook/pattern.h.
		const int kSlotRemove = 3 + StreamingVtableShift();       // strStreamingModule::Remove (unload)
		const int kSlotLoadComplete = 7 + StreamingVtableShift(); // strStreamingModule::SetResource

		constexpr size_t kDrawableNamePtr = 0xA8;
		constexpr size_t kDrawableLightsPtr = 0xB0;
		constexpr size_t kDrawableLightsCount = 0xB8;
		constexpr size_t kFragNamePtr = 0x58;
		constexpr size_t kFragLightsPtr = 0x110;
		constexpr size_t kFragLightsCount = 0x118;
		constexpr size_t kFragDrawablePtr = 0x30;
		constexpr size_t kFragDrawableArrayPtr = 0x38;
		constexpr size_t kFragDrawableArrayCount = 0x48;
		// Breakable props keep a drawable per physics child (pole, lamp head...),
		// and a part's lights live in that child's drawable. CodeWalker Frag.cs,
		// offsets validated against block sizes (FragPhysicsLOD = 304 bytes).
		constexpr size_t kFragPhysLodGroupPtr = 0xF0;
		constexpr size_t kPhysLodGroupLodPtr[3] = { 0x10, 0x18, 0x20 };
		constexpr size_t kPhysLodChildrenPtr = 0xD0;
		constexpr size_t kPhysLodChildrenCount = 0x11D;
		constexpr size_t kPhysLodSize = 304;
		constexpr size_t kPhysChildDrawable1 = 0xA0;
		constexpr size_t kPhysChildDrawable2 = 0xA8;
		constexpr size_t kPhysChildSize = 0x100;
		constexpr size_t kDictDrawablesPtr = 0x30;
		constexpr size_t kDictDrawablesCount = 0x38;

		constexpr size_t kLightAttrSize = 168;
		constexpr size_t kLightColour = 24;
		constexpr size_t kLightFlashiness = 27;
		constexpr size_t kLightVolumeColour = 84;

		constexpr size_t kTypesArchetypesArray = 24;
		constexpr size_t kTypesNameHash = 40;
		constexpr size_t kArchetypeExtensionsArray = 120;
		constexpr int kTypeIdSlotCandidates[2] = { 7, 1 };
		constexpr size_t kParStructureNameHash = 8;
		constexpr uint32_t kLightEffectHash = 0x27922C43u;
		constexpr size_t kLightEffectInstances = 32;
		constexpr size_t kLightDefSize = 160;
		constexpr size_t kLightDefColour = 20;
		constexpr size_t kLightDefFlashiness = 23;
		constexpr size_t kLightDefVolumeColour = 80;

		constexpr uint32_t kMaxLightsPerModel = 4096;
		constexpr uint32_t kMaxDrawablesPerContainer = 4096;
		constexpr uint32_t kMaxArchetypes = 65535;
		constexpr uint32_t kMaxExtensions = 256;
		constexpr size_t kEntityExtensionsArray = 96; // rage::fwEntityDef::extensions (atArray)
		constexpr uint32_t kMaxLightDefs = 256;

		using LoadCompleteFn = void* (*)(void* store, int32_t idx, void* a, void* b); // real signature beyond (this, idx) unknown; all registers pass through
		using FinishLoadingFn = void (*)(void* store, int32_t idx, void** obj);
		using GetTypeIdFn = void* (*)(void* self);

		struct StoreHook
		{
			track::Kind kind;
			const char* ext;
			const char* className;
			void* store = nullptr;
			LoadCompleteFn orig = nullptr;
			bool hooked = false;
		};
		StoreHook g_hooks[3] = {
			{ track::Ydr, "ydr", "DrawableStore" },
			{ track::Yft, "yft", "FragmentStore" },
			{ track::Ydd, "ydd", "DwdStore" },
		};

		constexpr int kMaxSiblings = 4;
		struct Sibling
		{
			void* target = nullptr;
			FinishLoadingFn orig = nullptr;
		};
		Sibling g_siblings[kMaxSiblings];
		int g_numSiblings = 0;
		void* g_ytypStore = nullptr;
		int g_typeIdSlot = -1;

		std::atomic<bool> g_available{ false };
		std::atomic<uint64_t> g_models{ 0 };
		std::atomic<uint64_t> g_entityBlocks{ 0 }; // map blocks with entity light-effect extensions
		std::atomic<uint64_t> g_lights{ 0 };
		std::atomic<uint64_t> g_recolored{ 0 };
		std::atomic<uint64_t> g_completeCalls{ 0 };
		std::atomic<uint64_t> g_typesLoaded{ 0 };
		std::atomic<uint64_t> g_typesWithLights{ 0 };
		uintptr_t g_base = 0;
		size_t g_size = 0;
		bool g_useVirtualQuery = true;
		bool g_useProbe = true;          // ReadProcessMemory probe when VirtualQuery is slow
		bool g_memChecksMeasured = false;

		// ------------------------------------------------------------ memory helpers

		template <typename T>
		T Read(const void* base, size_t off)
		{
			return *reinterpret_cast<const T*>(static_cast<const char*>(base) + off);
		}

		bool InImage(const void* p)
		{
			uintptr_t a = reinterpret_cast<uintptr_t>(p);
			return a >= g_base && a < g_base + g_size;
		}

		bool PlausiblePtr(const void* p)
		{
			uintptr_t a = reinterpret_cast<uintptr_t>(p);
			return a != 0 && (a & 7) == 0 && a > 0x10000 && a < 0x00007FFFFFFFFFFFull;
		}

		// Committed, readable memory covering [p, p+n)? VirtualQuery is timed
		// at init; if FiveM's anti-cheat makes it slow, this degrades to
		// pointer plausibility (the game's own invariants are trusted then).
		// Probe: reads one byte at p and at p+n-1 through the kernel, which
		// fails cleanly on unmapped memory instead of faulting.
		bool Probe(const void* p, size_t n)
		{
			uint8_t b = 0;
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), p, &b, 1, &got) || got != 1)
				return false;
			if (n > 1)
			{
				if (!ReadProcessMemory(GetCurrentProcess(), static_cast<const char*>(p) + n - 1, &b, 1, &got) || got != 1)
					return false;
			}
			return true;
		}

		bool Readable(const void* p, size_t n)
		{
			if (!p)
				return false;
			if (!g_useVirtualQuery)
			{
				if (!PlausiblePtr(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(p) & ~7ull)))
					return false;
				return g_useProbe ? Probe(p, n) : true;
			}
			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
				return false;
			if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
				return false;
			uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
			return reinterpret_cast<uintptr_t>(p) + n <= end;
		}

		// A live RAGE object: readable, with a vtable inside the game image.
		// This is the check that separates a real drawable / fragment /
		// physics child / extension from a garbage pointer.
		bool ObjectValid(const void* p, size_t minSize)
		{
			if (!PlausiblePtr(p) || !Readable(p, minSize))
				return false;
			return InImage(*static_cast<void* const* const*>(p));
		}

		// FiveM's anti-cheat intercepts VirtualQuery and, on heap addresses,
		// can make it cost over a millisecond; the same may apply to the
		// ReadProcessMemory probe. Both are timed once on a heap address.
		void MeasureMemoryChecks()
		{
			if (g_memChecksMeasured)
				return;
			g_memChecksMeasured = true;
			void* heap = HeapAlloc(GetProcessHeap(), 0, 64);
			if (!heap)
				return;
			LARGE_INTEGER f, t0, t1;
			QueryPerformanceFrequency(&f);

			QueryPerformanceCounter(&t0);
			MEMORY_BASIC_INFORMATION mbi{};
			for (int i = 0; i < 100; ++i)
				VirtualQuery(heap, &mbi, sizeof(mbi));
			QueryPerformanceCounter(&t1);
			double vq = 1e6 * double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart) / 100.0;

			QueryPerformanceCounter(&t0);
			for (int i = 0; i < 100; ++i)
				Probe(heap, 64);
			QueryPerformanceCounter(&t1);
			double pr = 1e6 * double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart) / 100.0;
			HeapFree(GetProcessHeap(), 0, heap);

			// VirtualQuery has measured anywhere from 4 us to 1.3 ms per call
			// between sessions (anti-cheat interception); the probe has been
			// under a microsecond every time. Prefer the probe.
			g_useProbe = pr < 20.0;
			g_useVirtualQuery = !g_useProbe && vq < 5.0;
			Log("near: memory checks: VirtualQuery %.1f us, ReadProcessMemory probe %.1f us -> %s", vq, pr,
				g_useProbe ? "probe" : (g_useVirtualQuery ? "VirtualQuery" : "vtable checks only"));
		}

		// Cfx hook::get_address: rel32 at p, relative to p+4.
		uintptr_t Rel32At(uintptr_t p)
		{
			return p + 4 + static_cast<uintptr_t>(static_cast<intptr_t>(*reinterpret_cast<const int32_t*>(p)));
		}

		// Cfx hook::get_call: p points at E8; rel32 at p+1, relative to p+5.
		uintptr_t CallTarget(uintptr_t p)
		{
			return p + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(*reinterpret_cast<const int32_t*>(p + 1)));
		}

		bool StartsWith(const char* str, const char* prefix)
		{
			for (int i = 0; i < 64; ++i)
			{
				if (prefix[i] == '\0')
					return true;
				if (str[i] != prefix[i])
					return false;
			}
			return false;
		}

		bool NameIs(const char* name, const char* want)
		{
			if (!name)
				return false;
			for (int i = 0; i < 32; ++i)
			{
				if (name[i] != want[i])
					return false;
				if (want[i] == '\0')
					return true;
			}
			return false;
		}

		uint32_t PackBytes(const uint8_t* p)
		{
			return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
		}

		void UnpackBytes(uint32_t v, uint8_t* p)
		{
			p[0] = uint8_t(v >> 16);
			p[1] = uint8_t(v >> 8);
			p[2] = uint8_t(v);
		}

		// Recolour one light record in place. Returns true if it changed.
		bool RecolourAt(uint8_t* rec, size_t colourOff, size_t flashOff, size_t volOff, const Config& cfg)
		{
			if (!cfg.nearEnabled || !cfg.match.enabled)
				return false;
			if (rec[flashOff] != 0) // strobes, flickers: leave alone
				return false;
			bool changed = false;
			for (size_t off : { colourOff, volOff })
			{
				uint8_t* c = rec + off;
				uint32_t packed = PackBytes(c);
				if (Recolor(packed, cfg.match))
				{
					UnpackBytes(packed, c);
					changed = true;
				}
			}
			return changed;
		}

		// ------------------------------------------------------------ store discovery

		bool LooksLikeStore(const void* p)
		{
			if (!PlausiblePtr(p) || !Readable(p, kPoolOffset + 24))
				return false;
			void* const* vt = *static_cast<void* const* const*>(p);
			if (!InImage(vt) || !Readable(vt, sizeof(void*) * (kSlotLoadComplete + 1)))
				return false;
			return InImage(vt[kSlotRemove]) && InImage(vt[kSlotLoadComplete]);
		}

		struct ModuleTable
		{
			void** modules = nullptr;
			uint32_t count = 0;
			size_t offset = 0;
		};

		bool FindModuleTable(const char* region, size_t span, void* known, ModuleTable& out)
		{
			if (!Readable(region, span))
				return false;
			for (size_t off = 0; off + 16 <= span; off += 8)
			{
				void** here = *reinterpret_cast<void** const*>(region + off);
				if (here == known)
				{
					void** start = reinterpret_cast<void**>(const_cast<char*>(region + off));
					void** first = start;
					while (first > reinterpret_cast<void**>(const_cast<char*>(region)) && LooksLikeStore(first[-1]))
						--first;
					void** last = start;
					while (reinterpret_cast<const char*>(last + 1) + 8 <= region + span && LooksLikeStore(last[1]))
						++last;
					out.modules = first;
					out.count = static_cast<uint32_t>(last - first + 1);
					out.offset = reinterpret_cast<const char*>(first) - region;
					return out.count >= 2;
				}
				uint16_t count = *reinterpret_cast<const uint16_t*>(region + off + 8);
				if (!PlausiblePtr(here) || count < 2 || count > 256 || !Readable(here, static_cast<size_t>(count) * sizeof(void*)))
					continue;
				for (uint32_t i = 0; i < count; ++i)
				{
					if (here[i] == known)
					{
						out.modules = here;
						out.count = count;
						out.offset = off;
						return true;
					}
				}
			}
			return false;
		}

		int FindNameOffset(const void* knownYmap)
		{
			const char* base = static_cast<const char*>(knownYmap);
			if (!Readable(base, kStoreNameOffsetMax))
				return -1;
			for (size_t off = 8; off + 8 <= kStoreNameOffsetMax; off += 8)
			{
				const char* str = *reinterpret_cast<const char* const*>(base + off);
				if ((!PlausiblePtr(str) && !InImage(str)) || !Readable(str, 16))
					continue;
				if (StartsWith(str, "MapDataStore"))
					return static_cast<int>(off);
			}
			return -1;
		}

		const char* ModuleName(const void* module, int nameOffset)
		{
			const char* str = *reinterpret_cast<const char* const*>(static_cast<const char*>(module) + nameOffset);
			if ((!PlausiblePtr(str) && !InImage(str)) || !Readable(str, 32))
				return nullptr;
			return str;
		}

		// The placed object for slot `idx`: pool entry +0. Read-only.
		void* PoolObject(void* store, uint32_t idx)
		{
			const char* pool = static_cast<const char*>(store) + kPoolOffset;
			char* data = *reinterpret_cast<char* const*>(pool);
			const int8_t* flags = *reinterpret_cast<const int8_t* const*>(pool + 8);
			uint32_t count = *reinterpret_cast<const uint32_t*>(pool + 16);
			uint32_t esz = *reinterpret_cast<const uint32_t*>(pool + 20);
			if (!data || !flags || idx >= count || esz < 16 || !Readable(flags + idx, 1) || flags[idx] < 0)
				return nullptr;
			const char* entry = data + static_cast<size_t>(idx) * esz;
			if (!Readable(entry, esz))
				return nullptr;
			void* obj = *reinterpret_cast<void* const*>(entry);
			if (!PlausiblePtr(obj) || !Readable(obj, 0x140) || !InImage(*static_cast<void* const*>(obj)))
				return nullptr;
			return obj;
		}

		// ------------------------------------------------------------ model lights

		void CollectDrawableLights(const void* drawable, std::vector<uint8_t*>& out)
		{
			if (!ObjectValid(drawable, kDrawableLightsCount + 2))
				return;
			uint8_t* lights = Read<uint8_t*>(drawable, kDrawableLightsPtr);
			uint16_t count = Read<uint16_t>(drawable, kDrawableLightsCount);
			if (!PlausiblePtr(lights) || count == 0 || count > kMaxLightsPerModel)
				return;
			if (!Readable(lights, static_cast<size_t>(count) * kLightAttrSize))
				return;
			for (uint16_t i = 0; i < count; ++i)
				out.push_back(lights + static_cast<size_t>(i) * kLightAttrSize);
		}

		void CollectLights(track::Kind kind, const void* obj, std::vector<uint8_t*>& out)
		{
			switch (kind)
			{
			case track::Ydr:
				CollectDrawableLights(obj, out);
				break;
			case track::Yft:
			{
				uint8_t* lights = Read<uint8_t*>(obj, kFragLightsPtr);
				uint16_t count = Read<uint16_t>(obj, kFragLightsCount);
				if (PlausiblePtr(lights) && count > 0 && count <= kMaxLightsPerModel
					&& Readable(lights, static_cast<size_t>(count) * kLightAttrSize))
					for (uint16_t i = 0; i < count; ++i)
						out.push_back(lights + static_cast<size_t>(i) * kLightAttrSize);

				CollectDrawableLights(Read<void*>(obj, kFragDrawablePtr), out);

				void** arr = Read<void**>(obj, kFragDrawableArrayPtr);
				uint32_t n = Read<uint32_t>(obj, kFragDrawableArrayCount);
				if (PlausiblePtr(arr) && n > 0 && n <= kMaxDrawablesPerContainer && Readable(arr, n * sizeof(void*)))
					for (uint32_t i = 0; i < n; ++i)
						CollectDrawableLights(arr[i], out);

				// physics children: each breakable part's own drawable(s)
				const void* group = Read<const void*>(obj, kFragPhysLodGroupPtr);
				if (ObjectValid(group, 0x28))
				{
					for (size_t lodOff : kPhysLodGroupLodPtr)
					{
						const void* lod = Read<const void*>(group, lodOff);
						if (!ObjectValid(lod, kPhysLodSize))
							continue;
						void** children = Read<void**>(lod, kPhysLodChildrenPtr);
						uint8_t nChildren = Read<uint8_t>(lod, kPhysLodChildrenCount);
						if (!PlausiblePtr(children) || nChildren == 0 || !Readable(children, nChildren * sizeof(void*)))
							continue;
						for (uint8_t c = 0; c < nChildren; ++c)
						{
							const void* child = children[c];
							if (!ObjectValid(child, kPhysChildSize))
								continue;
							CollectDrawableLights(Read<const void*>(child, kPhysChildDrawable1), out);
							CollectDrawableLights(Read<const void*>(child, kPhysChildDrawable2), out);
						}
					}
				}
				break;
			}
			case track::Ydd:
			{
				void** arr = Read<void**>(obj, kDictDrawablesPtr);
				uint16_t n = Read<uint16_t>(obj, kDictDrawablesCount);
				if (PlausiblePtr(arr) && n > 0 && n <= kMaxDrawablesPerContainer && Readable(arr, n * sizeof(void*)))
					for (uint16_t i = 0; i < n; ++i)
						CollectDrawableLights(arr[i], out);
				break;
			}
			default:
				break;
			}
			// the same light array can be reachable through more than one path
			std::vector<uint8_t*> unique;
			unique.reserve(out.size());
			for (uint8_t* p : out)
			{
				bool dup = false;
				for (uint8_t* q : unique)
					if (q == p)
					{
						dup = true;
						break;
					}
				if (!dup)
					unique.push_back(p);
			}
			out.swap(unique);
		}

		const char* ModelName(track::Kind kind, const void* obj, char* buf, size_t bufSize)
		{
			const char* namePtr = nullptr;
			if (kind == track::Ydr)
				namePtr = Read<const char*>(obj, kDrawableNamePtr);
			else if (kind == track::Yft)
				namePtr = Read<const char*>(obj, kFragNamePtr);
			buf[0] = '?';
			buf[1] = 0;
			if (!namePtr || !Readable(namePtr, bufSize))
				return buf;
			size_t i = 0;
			for (; i + 1 < bufSize && namePtr[i]; ++i)
				buf[i] = (namePtr[i] >= 32 && namePtr[i] < 127) ? namePtr[i] : '.';
			buf[i] = 0;
			return buf;
		}

		bool RepaintModel(track::Kind kind, void* obj, const std::vector<uint32_t>& originals, const Config& cfg, uint64_t& lights, uint64_t& changed)
		{
			std::vector<uint8_t*> attrs;
			CollectLights(kind, obj, attrs);
			if (attrs.size() * 2 != originals.size())
				return false;
			for (size_t i = 0; i < attrs.size(); ++i)
			{
				UnpackBytes(originals[i * 2], attrs[i] + kLightColour);
				UnpackBytes(originals[i * 2 + 1], attrs[i] + kLightVolumeColour);
				if (RecolourAt(attrs[i], kLightColour, kLightFlashiness, kLightVolumeColour, cfg))
					changed++;
			}
			lights += attrs.size();
			return true;
		}

		bool RepaintYdr(void* o, const std::vector<uint32_t>& r, const Config& c, uint64_t& l, uint64_t& ch) { return RepaintModel(track::Ydr, o, r, c, l, ch); }
		bool RepaintYft(void* o, const std::vector<uint32_t>& r, const Config& c, uint64_t& l, uint64_t& ch) { return RepaintModel(track::Yft, o, r, c, l, ch); }
		bool RepaintYdd(void* o, const std::vector<uint32_t>& r, const Config& c, uint64_t& l, uint64_t& ch) { return RepaintModel(track::Ydd, o, r, c, l, ch); }

		// Session-wide tally of light colours that matched nothing, kept for the
		// warm range only (hue 10..60), reported on the F9 stats line so the
		// remaining "orange" is named by its RGB.
		SRWLOCK g_unmatchedLock = SRWLOCK_INIT;
		struct UnmatchedColour { uint32_t rgb; uint32_t count; };
		UnmatchedColour g_unmatched[32];
		int g_unmatchedN = 0;

		void NoteUnmatched(uint32_t rgb)
		{
			RGB c{ float(rgb >> 16 & 0xFF), float(rgb >> 8 & 0xFF), float(rgb & 0xFF) };
			HSV h = ToHSV(c);
			if (h.v < 0.2f || h.h < 10.f || h.h > 60.f)
				return;
			AcquireSRWLockExclusive(&g_unmatchedLock);
			int i = 0;
			for (; i < g_unmatchedN; ++i)
				if (g_unmatched[i].rgb == rgb)
					break;
			if (i == g_unmatchedN && g_unmatchedN < 32)
			{
				g_unmatched[g_unmatchedN] = { rgb, 0 };
				g_unmatchedN++;
			}
			if (i < 32)
				g_unmatched[i].count++;
			ReleaseSRWLockExclusive(&g_unmatchedLock);
		}

		// "(r,g,b)xN (r,g,b)xN ..." for up to 6 distinct colours in a light set.
		std::string ColourBreakdown(const std::vector<uint32_t>& originals)
		{
			uint32_t cols[6];
			uint32_t counts[6];
			int n = 0;
			int more = 0;
			for (size_t i = 0; i < originals.size(); i += 2)
			{
				uint32_t rgb = originals[i];
				int k = 0;
				for (; k < n; ++k)
					if (cols[k] == rgb)
						break;
				if (k == n)
				{
					if (n == 6) { more++; continue; }
					cols[n] = rgb;
					counts[n] = 0;
					n++;
				}
				counts[k]++;
			}
			std::string out;
			char buf[48];
			for (int k = 0; k < n; ++k)
			{
				RGB c{ float(cols[k] >> 16 & 0xFF), float(cols[k] >> 8 & 0xFF), float(cols[k] & 0xFF) };
				HSV h = ToHSV(c);
				snprintf(buf, sizeof(buf), " (%u,%u,%u h%.0f s%.2f)x%u", cols[k] >> 16 & 0xFF, cols[k] >> 8 & 0xFF, cols[k] & 0xFF, h.h, h.s, counts[k]);
				out += buf;
			}
			if (more)
				out += " +more";
			return out;
		}

		// Debug: the textures embedded in a placed model (drawable +0x10 =
		// grmShaderGroup, +0x08 = its pgDictionary<grcTexture>; fragment's
		// drawable at +0x30). Lens glow textures that live here never pass
		// through the texture store.
		std::string EmbeddedTextures(track::Kind kind, const void* obj)
		{
			const void* drawable = kind == track::Yft ? Read<const void*>(obj, kFragDrawablePtr) : (kind == track::Ydr ? obj : nullptr);
			if (!ObjectValid(drawable, 0xC0))
				return " (no drawable)";
			const void* shaderGroup = Read<const void*>(drawable, 0x10);
			if (!ObjectValid(shaderGroup, 0x40))
				return " (no shader group)";
			const void* txd = Read<const void*>(shaderGroup, 0x08);
			if (!txd)
				return " (none)";
			if (!ObjectValid(txd, 0x40))
				return " (txd unreadable)";
			const void* const* entries = Read<const void* const*>(txd, 0x30);
			const uint16_t count = Read<uint16_t>(txd, 0x38);
			if (!PlausiblePtr(entries) || count == 0 || count > 256 || !Readable(entries, count * sizeof(void*)))
				return " (empty)";
			std::string out;
			char buf[160];
			for (uint16_t i = 0; i < count && i < 16; ++i)
			{
				const void* tex = entries[i];
				if (!ObjectValid(tex, 0x90))
				{
					out += " [bad]";
					continue;
				}
				const char* name = Read<const char*>(tex, 0x28);
				const uint16_t w = Read<uint16_t>(tex, 0x50), hgt = Read<uint16_t>(tex, 0x52);
				const uint32_t fmt = Read<uint32_t>(tex, 0x58);
				char fname[8] = "fmt?";
				if ((fmt >> 24) >= 0x20 && (fmt & 0xFF) >= 0x20)
					snprintf(fname, sizeof(fname), "%c%c%c%c", fmt & 0xFF, (fmt >> 8) & 0xFF, (fmt >> 16) & 0xFF, fmt >> 24);
				else
					snprintf(fname, sizeof(fname), "f%u", fmt);
				snprintf(buf, sizeof(buf), " %s(%ux%u %s)", (PlausiblePtr(name) && Readable(name, 48)) ? name : "?", w, hgt, fname);
				out += buf;
			}
			return out;
		}

		void OnModelLoaded(StoreHook& h, uint32_t idx, void* obj)
		{
			if (!obj)
				return;
			if (DebugLogging() && h.kind != track::Ydd)
			{
				char name[48];
				ModelName(h.kind, obj, name, sizeof(name));
				for (const char* k : { "streetlight", "lamp", "light" })
					if (strstr(name, k))
					{
						LogDebug("near: %s '%s' embedded textures:%s", h.ext, name, EmbeddedTextures(h.kind, obj).c_str());
						break;
					}
			}
			std::vector<uint8_t*> attrs;
			CollectLights(h.kind, obj, attrs);
			if (attrs.empty())
			{
				if (DebugLogging())
				{
					char name[48];
					ModelName(h.kind, obj, name, sizeof(name));
					for (const char* k : { "streetlight", "traffic", "lamp" })
						if (strstr(name, k))
						{
							LogDebug("near: %s slot %u '%s' has NO lights reachable", h.ext, idx, name);
							break;
						}
				}
				return;
			}

			const Config cfg = GetConfig();
			std::vector<uint32_t> originals;
			originals.reserve(attrs.size() * 2);
			uint32_t changed = 0;
			for (uint8_t* attr : attrs)
			{
				const uint32_t before = PackBytes(attr + kLightColour);
				originals.push_back(before);
				originals.push_back(PackBytes(attr + kLightVolumeColour));
				if (RecolourAt(attr, kLightColour, kLightFlashiness, kLightVolumeColour, cfg))
					changed++;
				else if (attr[kLightFlashiness] == 0)
					NoteUnmatched(before);
			}
			g_models++;
			g_lights += attrs.size();
			g_recolored += changed;

			if (cfg.debug && cfg.nearLog)
			{
				char name[48];
				LogDebug("near: %s slot %u '%s' lights=%u recolored=%u colours:%s",
					h.ext, idx, ModelName(h.kind, obj, name, sizeof(name)), (unsigned)attrs.size(), changed, ColourBreakdown(originals).c_str());
			}

			if (cfg.liveRepaint)
				track::Register(h.kind, h.store, idx, obj, std::move(originals));
		}

		// The game creates the per-entity light objects for a model inside its
		// load-completion (copying colours), so the recolour must happen
		// BEFORE the original runs. The model is already placed by then; if
		// its pool entry is somehow not yet filled, fall back to afterwards.
		template <int K>
		void* CompleteThunk(void* store, int32_t idx, void* a, void* b)
		{
			StoreHook* mine = nullptr;
			if (idx >= 0)
				for (StoreHook& h : g_hooks)
					if (h.store && store == h.store)
						mine = &h;

			bool done = false;
			if (mine)
			{
				void* obj = PoolObject(mine->store, static_cast<uint32_t>(idx));
				if (obj)
				{
					OnModelLoaded(*mine, static_cast<uint32_t>(idx), obj);
					done = true;
				}
			}

			void* r = g_hooks[K].orig(store, idx, a, b);

			const uint64_t n = ++g_completeCalls;
			if (n <= 3)
				LogDebug("near: load-complete #%llu: store=%p idx=%d (recoloured %s)", (unsigned long long)n, store, idx, done ? "before" : "after/none");
			if (mine && !done)
				OnModelLoaded(*mine, static_cast<uint32_t>(idx), PoolObject(mine->store, static_cast<uint32_t>(idx)));
			return r;
		}

		void* CompleteThunkFor(int k)
		{
			switch (k)
			{
			case 0: return reinterpret_cast<void*>(&CompleteThunk<0>);
			case 1: return reinterpret_cast<void*>(&CompleteThunk<1>);
			default: return reinterpret_cast<void*>(&CompleteThunk<2>);
			}
		}

		// ------------------------------------------------------------ archetype lights

		// parStructure* of an extension object, or nullptr. The vtable slot is
		// chosen on first use: the right one returns a readable descriptor
		// (heap-allocated, not in the image) with a name hash, a null or
		// plausible base class at +16 and a sane member count at +48.
		const char* ExtensionType(const void* ext)
		{
			void* const* vt = *static_cast<void* const* const*>(ext);
			if (!InImage(vt) || !Readable(vt, sizeof(void*) * 8))
				return nullptr;
			if (g_typeIdSlot < 0)
			{
				for (int slot : kTypeIdSlotCandidates)
				{
					if (!InImage(vt[slot]))
						continue;
					const char* t = static_cast<const char*>(reinterpret_cast<GetTypeIdFn>(vt[slot])(const_cast<void*>(ext)));
					if (PlausiblePtr(t) && Readable(t, 64)
						&& *reinterpret_cast<const uint32_t*>(t + kParStructureNameHash) != 0
						&& (*reinterpret_cast<void* const*>(t + 16) == nullptr || PlausiblePtr(*reinterpret_cast<void* const*>(t + 16)))
						&& *reinterpret_cast<const uint16_t*>(t + 48 + 8) < 512)
					{
						g_typeIdSlot = slot;
						Log("near: extension type getter is vtable slot %d", slot);
						break;
					}
				}
				if (g_typeIdSlot < 0)
					return nullptr;
			}
			if (!InImage(vt[g_typeIdSlot]))
				return nullptr;
			const char* t = static_cast<const char*>(reinterpret_cast<GetTypeIdFn>(vt[g_typeIdSlot])(const_cast<void*>(ext)));
			return (PlausiblePtr(t) && Readable(t, 16)) ? t : nullptr;
		}

		// Every light-effect CLightAttrDef reachable from a CMapTypes, in a
		// deterministic order. The optional survey arrays collect the
		// extension type hashes met.
		void CollectTypeLights(const void* mapTypes, std::vector<uint8_t*>& out, uint32_t* surveyHashes, uint32_t* surveyCounts, int* surveyDistinct)
		{
			if (!ObjectValid(mapTypes, 48))
				return;
			void** archetypes = Read<void**>(mapTypes, kTypesArchetypesArray);
			uint16_t nArch = Read<uint16_t>(mapTypes, kTypesArchetypesArray + 8);
			if (!PlausiblePtr(archetypes) || nArch == 0 || nArch > kMaxArchetypes || !Readable(archetypes, nArch * sizeof(void*)))
				return;
			for (uint16_t a = 0; a < nArch; ++a)
			{
				const void* arch = archetypes[a];
				if (!ObjectValid(arch, kArchetypeExtensionsArray + 16))
					continue;
				void** exts = Read<void**>(arch, kArchetypeExtensionsArray);
				uint16_t nExt = Read<uint16_t>(arch, kArchetypeExtensionsArray + 8);
				if (!PlausiblePtr(exts) || nExt == 0 || nExt > kMaxExtensions || !Readable(exts, nExt * sizeof(void*)))
					continue;
				for (uint16_t e = 0; e < nExt; ++e)
				{
					const void* ext = exts[e];
					if (!ObjectValid(ext, 48))
						continue;
					const char* type = ExtensionType(ext);
					if (!type)
						continue;
					const uint32_t hash = *reinterpret_cast<const uint32_t*>(type + kParStructureNameHash);
					if (surveyHashes)
					{
						int k = 0;
						for (; k < *surveyDistinct; ++k)
							if (surveyHashes[k] == hash)
								break;
						if (k == *surveyDistinct && *surveyDistinct < 16)
						{
							surveyHashes[*surveyDistinct] = hash;
							surveyCounts[*surveyDistinct] = 0;
							(*surveyDistinct)++;
						}
						if (k < 16)
							surveyCounts[k]++;
					}
					if (hash != kLightEffectHash)
						continue;
					uint8_t* defs = Read<uint8_t*>(ext, kLightEffectInstances);
					uint16_t nDefs = Read<uint16_t>(ext, kLightEffectInstances + 8);
					if (!PlausiblePtr(defs) || nDefs == 0 || nDefs > kMaxLightDefs || !Readable(defs, static_cast<size_t>(nDefs) * kLightDefSize))
						continue;
					for (uint16_t i = 0; i < nDefs; ++i)
						out.push_back(defs + static_cast<size_t>(i) * kLightDefSize);
				}
			}
		}

		bool RepaintYtyp(void* obj, const std::vector<uint32_t>& originals, const Config& cfg, uint64_t& lights, uint64_t& changed)
		{
			std::vector<uint8_t*> defs;
			CollectTypeLights(obj, defs, nullptr, nullptr, nullptr);
			if (defs.size() * 2 != originals.size())
				return false;
			for (size_t i = 0; i < defs.size(); ++i)
			{
				UnpackBytes(originals[i * 2], defs[i] + kLightDefColour);
				UnpackBytes(originals[i * 2 + 1], defs[i] + kLightDefVolumeColour);
				if (RecolourAt(defs[i], kLightDefColour, kLightDefFlashiness, kLightDefVolumeColour, cfg))
					changed++;
			}
			lights += defs.size();
			return true;
		}

		void OnTypesLoaded(void* store, uint32_t idx, void* mapTypes)
		{
			g_typesLoaded++;
			if (!ObjectValid(mapTypes, 48))
				return;

			// survey of extension types over the first 40 files, logged once
			static uint32_t sHashes[16], sCounts[16];
			static int sDistinct = 0, sFiles = 0;
			static bool sDone = false;
			const bool survey = !sDone;

			std::vector<uint8_t*> defs;
			CollectTypeLights(mapTypes, defs, survey ? sHashes : nullptr, survey ? sCounts : nullptr, survey ? &sDistinct : nullptr);
			if (survey && ++sFiles >= 40)
			{
				sDone = true;
				std::string txt;
				char buf[40];
				for (int k = 0; k < sDistinct; ++k)
				{
					snprintf(buf, sizeof(buf), " %08X x%u", sHashes[k], sCounts[k]);
					txt += buf;
				}
				Log("near: archetype extension types over the first %d ytyp files:%s  (light effect = %08X)", sFiles, txt.c_str(), kLightEffectHash);
			}
			if (defs.empty())
				return;
			g_typesWithLights++;

			const Config cfg = GetConfig();
			std::vector<uint32_t> originals;
			originals.reserve(defs.size() * 2);
			uint32_t changed = 0;
			for (uint8_t* d : defs)
			{
				originals.push_back(PackBytes(d + kLightDefColour));
				originals.push_back(PackBytes(d + kLightDefVolumeColour));
				if (RecolourAt(d, kLightDefColour, kLightDefFlashiness, kLightDefVolumeColour, cfg))
					changed++;
			}
			g_models++;
			g_lights += defs.size();
			g_recolored += changed;

			if (cfg.debug && cfg.nearLog)
			{
				RGB c0{ float(originals[0] >> 16 & 0xFF), float(originals[0] >> 8 & 0xFF), float(originals[0] & 0xFF) };
				HSV h0 = ToHSV(c0);
				LogDebug("near: ytyp slot %u name=%08X lights=%u recolored=%u light[0] was (%.0f,%.0f,%.0f) hue=%.1f sat=%.2f%s",
					idx, Read<uint32_t>(mapTypes, kTypesNameHash), (unsigned)defs.size(), changed, c0.r, c0.g, c0.b, h0.h, h0.s,
					changed == 0 ? "  [no light matched]" : "");
			}

			if (cfg.liveRepaint)
				track::Register(track::Ytyp, store, idx, mapTypes, std::move(originals));
		}

		template <int I>
		void TypesThunk(void* store, int32_t idx, void** obj)
		{
			g_siblings[I].orig(store, idx, obj);
			if (idx < 0 || !obj)
				return;
			// The sibling is the archetype store's own FinishLoading, so the
			// first store seen through it is the archetype store.
			if (!g_ytypStore)
			{
				g_ytypStore = store;
				Log("near: archetype store is %p (first FinishLoading through the sibling hook)", store);
			}
			if (store == g_ytypStore)
				OnTypesLoaded(store, static_cast<uint32_t>(idx), *obj);
		}

		void* TypesThunkFor(int i)
		{
			switch (i)
			{
			case 0: return reinterpret_cast<void*>(&TypesThunk<0>);
			case 1: return reinterpret_cast<void*>(&TypesThunk<1>);
			case 2: return reinterpret_cast<void*>(&TypesThunk<2>);
			default: return reinterpret_cast<void*>(&TypesThunk<3>);
			}
		}

		// Every `call` to the known ymap FinishLoading, found by scanning the
		// image; the 12 bytes before such a call plus E8 identify the sibling
		// call sites (two on b3751). Returns the targets other than `known`.
		std::vector<void*> FindSiblingFinishLoading(void* known)
		{
			std::vector<void*> out;
			std::vector<uintptr_t> sites;
			const uint8_t* img = reinterpret_cast<const uint8_t*>(g_base);
			for (size_t i = 0; i + 5 <= g_size && sites.size() < 8; ++i)
			{
				if ((i & 0xFFF) == 0 && !Readable(img + i, 0x1000))
				{
					i += 0xFFF;
					continue;
				}
				if (img[i] != 0xE8)
					continue;
				if (i >= 12 && CallTarget(g_base + i) == reinterpret_cast<uintptr_t>(known))
					sites.push_back(g_base + i);
			}
			Log("near: %u call sites of the ymap FinishLoading found", (unsigned)sites.size());
			for (uintptr_t call : sites)
			{
				std::string text;
				char buf[8];
				const uint8_t* b = reinterpret_cast<const uint8_t*>(call - 12);
				for (int i = 0; i < 12; ++i)
				{
					snprintf(buf, sizeof(buf), "%02X ", b[i]);
					text += buf;
				}
				text += "E8";
				Pattern pat;
				if (!ParsePattern(text, pat))
					continue;
				for (uintptr_t h : FindPattern(pat, g_base, g_size, 16))
				{
					void* t = reinterpret_cast<void*>(CallTarget(h + 12));
					if (t == known || !InImage(t))
						continue;
					bool dup = false;
					for (void* x : out)
						if (x == t)
							dup = true;
					if (!dup)
						out.push_back(t);
				}
			}
			Log("near: sibling FinishLoading candidates: %u", (unsigned)out.size());
			return out;
		}
	}

	// ------------------------------------------------------------------ public

	bool InstallTypesHook(uintptr_t imageBase, size_t imageSize, void* knownFinishLoading)
	{
		g_base = imageBase;
		g_size = imageSize;
		MeasureMemoryChecks();
		int got = 0;
		for (void* t : FindSiblingFinishLoading(knownFinishLoading))
		{
			if (g_numSiblings >= kMaxSiblings)
				break;
			Sibling& sib = g_siblings[g_numSiblings];
			MH_STATUS st = MH_CreateHook(t, TypesThunkFor(g_numSiblings), reinterpret_cast<void**>(&sib.orig));
			if (st == MH_OK)
				st = MH_EnableHook(t);
			if (st != MH_OK)
			{
				Log("near: detouring sibling FinishLoading at base+0x%llX failed: %s", (unsigned long long)(reinterpret_cast<uintptr_t>(t) - g_base), MH_StatusToString(st));
				sib.orig = nullptr;
				continue;
			}
			sib.target = t;
			g_numSiblings++;
			got++;
			Log("near: sibling FinishLoading at base+0x%llX detoured (archetype lights)", (unsigned long long)(reinterpret_cast<uintptr_t>(t) - g_base));
		}
		if (got == 0)
			Log("near: no sibling FinishLoading hooked; archetype lights unavailable");
		track::SetRepaint(track::Ytyp, &RepaintYtyp);
		return got > 0;
	}

	bool Init(uintptr_t imageBase, size_t imageSize, void* knownYmapStore)
	{
		g_base = imageBase;
		g_size = imageSize;

		MeasureMemoryChecks();

		if (!LooksLikeStore(knownYmapStore))
		{
			Log("near: the ymap store %p from the map hook does not look like a streaming module; giving up", knownYmapStore);
			return false;
		}

		// 1. streaming manager + module table
		Pattern pat;
		if (!ParsePattern(kManagerPattern, pat))
			return false;
		ModuleTable table;
		void* manager = nullptr;
		for (uintptr_t hit : FindPattern(pat, g_base, g_size, 8))
		{
			void* mgr = reinterpret_cast<void*>(Rel32At(hit + kManagerOffset));
			if (!InImage(mgr))
				continue;
			if (FindModuleTable(static_cast<const char*>(mgr), kManagerScanSpan, knownYmapStore, table))
			{
				manager = mgr;
				break;
			}
		}
		if (!manager)
		{
			Log("near: no streaming manager candidate holds a module table containing the ymap store; model-light hooks not installed");
			return false;
		}
		Log("near: streaming manager %p (base+0x%llX), %u modules at +0x%X", manager,
			(unsigned long long)(reinterpret_cast<uintptr_t>(manager) - g_base), table.count, (unsigned)table.offset);

		// 2. module names
		const int nameOffset = FindNameOffset(knownYmapStore);
		if (nameOffset < 0)
		{
			Log("near: no field of the ymap store points at \"MapDataStore\"; model-light hooks not installed");
			return false;
		}
		{
			std::string names;
			for (uint32_t i = 0; i < table.count; ++i)
			{
				const char* n = LooksLikeStore(table.modules[i]) ? ModuleName(table.modules[i], nameOffset) : nullptr;
				names += ' ';
				names += n ? n : "?";
			}
			Log("near: modules:%s", names.c_str());
		}

		// 3. hook the load-complete slot on the three model stores
		int hooked = 0;
		for (int k = 0; k < 3; ++k)
		{
			StoreHook& h = g_hooks[k];
			for (uint32_t i = 0; i < table.count; ++i)
			{
				void* m = table.modules[i];
				if (LooksLikeStore(m) && NameIs(ModuleName(m, nameOffset), h.className))
				{
					h.store = m;
					break;
				}
			}
			if (!h.store)
			{
				Log("near: no '%s' (%s) module found; skipping", h.ext, h.className);
				continue;
			}

			void** vt = *static_cast<void***>(h.store);
			void* fn = vt[kSlotLoadComplete];
			MH_STATUS st = MH_CreateHook(fn, CompleteThunkFor(k), reinterpret_cast<void**>(&h.orig));
			if (st == MH_ERROR_ALREADY_CREATED)
			{
				h.hooked = true;
				hooked++;
				Log("near: '%s' store %p shares the load-complete stub (already detoured)", h.ext, h.store);
				continue;
			}
			if (st == MH_OK)
				st = MH_EnableHook(fn);
			if (st != MH_OK)
			{
				Log("near: detouring '%s' load-complete at %p failed: %s", h.ext, fn, MH_StatusToString(st));
				h.orig = nullptr;
				h.store = nullptr;
				continue;
			}
			h.hooked = true;
			hooked++;
			Log("near: '%s' store %p: load-complete vtable[%d] at %p (base+0x%llX) detoured",
				h.ext, h.store, kSlotLoadComplete, fn, (unsigned long long)(reinterpret_cast<uintptr_t>(fn) - g_base));
		}

		// Texture dictionaries: the lantern glow lives there (see textures.h).
		if (GetConfig().texturesEnabled)
		{
			void* txd = nullptr;
			for (uint32_t i = 0; i < table.count; ++i)
			{
				void* m = table.modules[i];
				if (LooksLikeStore(m) && NameIs(ModuleName(m, nameOffset), "TxdStore"))
				{
					txd = m;
					break;
				}
			}
			const int placeSlot = 6 + StreamingVtableShift(); // strStreamingModule::PlaceResource
			if (txd && textures::InstallPlacementHook(textures::Txd, txd, placeSlot, g_base, g_size))
				track::WatchStore(track::Txd, txd);
			else
				Log("tex: no 'TxdStore' module found; lantern textures not hooked");
			// Most lamp props embed their lens texture in the model itself, so the
			// model stores' placement is hooked the same way.
			const textures::StoreKind kinds[3] = { textures::Ydr, textures::Yft, textures::Ydd };
			for (int k = 0; k < 3; ++k)
				if (g_hooks[k].store && g_hooks[k].hooked)
					textures::InstallPlacementHook(kinds[k], g_hooks[k].store, placeSlot, g_base, g_size);
			track::SetRemoveListener(&textures::OnStoreRemove);
		}

		track::SetRepaint(track::Ydr, &RepaintYdr);
		track::SetRepaint(track::Yft, &RepaintYft);
		track::SetRepaint(track::Ydd, &RepaintYdd);

		// 4. Models resident before this hook existed never pass through it.
		//    One pass over each store's pool (usually a handful of models).
		for (StoreHook& h : g_hooks)
		{
			if (!h.store || !h.hooked)
				continue;
			// No per-slot memory probes here: on the pool's pages the kernel
			// checks cost milliseconds each under the anti-cheat and 235k slots
			// looked like a hang. The pool is the game's own structure; per
			// slot only pointer sanity + a vtable inside the game image.
			const char* pool = static_cast<const char*>(h.store) + kPoolOffset;
			const char* data = *reinterpret_cast<const char* const*>(pool);
			const int8_t* flags = *reinterpret_cast<const int8_t* const*>(pool + 8);
			uint32_t count = *reinterpret_cast<const uint32_t*>(pool + 16);
			uint32_t esz = *reinterpret_cast<const uint32_t*>(pool + 20);
			if (!data || !flags || esz < 16 || !Readable(flags, count) || !Readable(data, static_cast<size_t>(count) * esz))
				continue;
			uint32_t loaded = 0;
			const uint64_t before = g_models.load();
			for (uint32_t idx = 0; idx < count; ++idx)
			{
				if (flags[idx] < 0)
					continue;
				void* obj = *reinterpret_cast<void* const*>(data + static_cast<size_t>(idx) * esz);
				if (!PlausiblePtr(obj) || !InImage(*static_cast<void* const*>(obj)))
					continue;
				loaded++;
				if (DebugLogging())
				{
					char name[48];
					LogDebug("near: '%s' already loaded at hook time: slot %u '%s'", h.ext, idx, ModelName(h.kind, obj, name, sizeof(name)));
				}
				OnModelLoaded(h, idx, obj);
			}
			Log("near: '%s' initial sweep: %u models already loaded, %llu with lights", h.ext, loaded, (unsigned long long)(g_models.load() - before));
		}

		g_available = hooked > 0;
		return hooked > 0;
	}

	// Every light-effect CLightAttrDef on the entities of a map block, in a
	// deterministic order. Runs inside the ymap FinishLoading hook, so the
	// entity pointers are the freshly loaded resource's own.
	// `validate` = every pointer is probed before use: the repaint path runs
	// from the menu thread long after the block loaded, and the game may have
	// released or reused parts of it by then (that crashed once, 0.15.0).
	static bool CollectEntityLights(void* const* entities, uint32_t count, std::vector<uint8_t*>& out, bool validate)
	{
		if (!entities || count == 0 || count > 100000)
			return true;
		if (validate && (!PlausiblePtr(entities) || !Readable(entities, static_cast<size_t>(count) * sizeof(void*))))
			return false;
		for (uint32_t i = 0; i < count; ++i)
		{
			const uint8_t* e = static_cast<const uint8_t*>(entities[i]);
			if (!e)
				continue;
			if (validate && !ObjectValid(e, kEntityExtensionsArray + 16))
				return false;
			const uint16_t nExt = *reinterpret_cast<const uint16_t*>(e + kEntityExtensionsArray + 8);
			if (nExt == 0 || nExt > kMaxExtensions)
				continue;
			void* const* exts = *reinterpret_cast<void* const* const*>(e + kEntityExtensionsArray);
			if (!PlausiblePtr(exts) || !Readable(exts, nExt * sizeof(void*)))
				continue;
			for (uint16_t x = 0; x < nExt; ++x)
			{
				const void* ext = exts[x];
				if (!ObjectValid(ext, 48))
					continue;
				const char* type = ExtensionType(ext);
				if (!type || *reinterpret_cast<const uint32_t*>(type + kParStructureNameHash) != kLightEffectHash)
					continue;
				uint8_t* defs = Read<uint8_t*>(ext, kLightEffectInstances);
				uint16_t nDefs = Read<uint16_t>(ext, kLightEffectInstances + 8);
				if (!PlausiblePtr(defs) || nDefs == 0 || nDefs > kMaxLightDefs || !Readable(defs, static_cast<size_t>(nDefs) * kLightDefSize))
					continue;
				for (uint16_t d = 0; d < nDefs; ++d)
					out.push_back(defs + static_cast<size_t>(d) * kLightDefSize);
			}
		}
		return true;
	}

	uint32_t RecolourEntityLights(uint32_t mapName, void* const* entities, uint32_t count, const Config& cfg, std::vector<uint32_t>& originals)
	{
		if (!cfg.nearEnabled)
			return 0;
		std::vector<uint8_t*> defs;
		CollectEntityLights(entities, count, defs, false);
		if (defs.empty())
			return 0;
		uint32_t changed = 0;
		std::vector<uint32_t> before;
		before.reserve(defs.size() * 2);
		for (uint8_t* d : defs)
		{
			const uint32_t col = PackBytes(d + kLightDefColour);
			before.push_back(col);
			before.push_back(PackBytes(d + kLightDefVolumeColour));
			if (RecolourAt(d, kLightDefColour, kLightDefFlashiness, kLightDefVolumeColour, cfg))
				changed++;
			else if (d[kLightDefFlashiness] == 0)
				NoteUnmatched(col);
		}
		g_entityBlocks++;
		g_lights += defs.size();
		g_recolored += changed;
		if (cfg.debug && cfg.nearLog)
		{
			LogDebug("near: ymap %08x entity lights=%u recolored=%u colours:%s", mapName, (unsigned)defs.size(), changed, ColourBreakdown(before).c_str());
		}
		originals.insert(originals.end(), before.begin(), before.end());
		return static_cast<uint32_t>(defs.size());
	}

	bool RepaintEntityLights(void* const* entities, uint32_t count, const std::vector<uint32_t>& originals, size_t offset, const Config& cfg, uint64_t& lights, uint64_t& changed)
	{
		std::vector<uint8_t*> defs;
		if (!CollectEntityLights(entities, count, defs, true))
			return false;
		if (originals.size() != offset + defs.size() * 2)
			return false;
		for (size_t i = 0; i < defs.size(); ++i)
		{
			UnpackBytes(originals[offset + i * 2], defs[i] + kLightDefColour);
			UnpackBytes(originals[offset + i * 2 + 1], defs[i] + kLightDefVolumeColour);
			if (RecolourAt(defs[i], kLightDefColour, kLightDefFlashiness, kLightDefVolumeColour, cfg))
				changed++;
		}
		lights += defs.size();
		return true;
	}

	uint64_t EntityBlocks() { return g_entityBlocks.load(); }

	std::string DescribeExtensions(void* const* exts, unsigned n)
	{
		std::string out;
		char buf[96];
		if (!PlausiblePtr(exts) || n == 0 || n > kMaxExtensions || !Readable(exts, n * sizeof(void*)))
			return out;
		for (unsigned e = 0; e < n; ++e)
		{
			const void* ext = exts[e];
			if (!ObjectValid(ext, 48))
			{
				out += " [bad ext]";
				continue;
			}
			const char* type = ExtensionType(ext);
			if (!type)
			{
				out += " [ext type unknown]";
				continue;
			}
			const uint32_t hash = *reinterpret_cast<const uint32_t*>(type + kParStructureNameHash);
			snprintf(buf, sizeof(buf), " [ext %08X", hash);
			out += buf;
			if (hash == kLightEffectHash)
			{
				uint8_t* defs = Read<uint8_t*>(ext, kLightEffectInstances);
				uint16_t nDefs = Read<uint16_t>(ext, kLightEffectInstances + 8);
				if (PlausiblePtr(defs) && nDefs > 0 && nDefs <= kMaxLightDefs && Readable(defs, static_cast<size_t>(nDefs) * kLightDefSize))
				{
					for (uint16_t i = 0; i < nDefs; ++i)
					{
						const uint8_t* d = defs + static_cast<size_t>(i) * kLightDefSize;
						snprintf(buf, sizeof(buf), " light(%u,%u,%u flash=%u)", d[kLightDefColour], d[kLightDefColour + 1], d[kLightDefColour + 2], d[kLightDefFlashiness]);
						out += buf;
					}
				}
			}
			out += "]";
		}
		return out;
	}

	std::string UnmatchedWarmSummary()
	{
		std::string out;
		char buf[48];
		AcquireSRWLockShared(&g_unmatchedLock);
		for (int i = 0; i < g_unmatchedN; ++i)
		{
			RGB c{ float(g_unmatched[i].rgb >> 16 & 0xFF), float(g_unmatched[i].rgb >> 8 & 0xFF), float(g_unmatched[i].rgb & 0xFF) };
			HSV h = ToHSV(c);
			snprintf(buf, sizeof(buf), " (%.0f,%.0f,%.0f h%.0f s%.2f)x%u", c.r, c.g, c.b, h.h, h.s, g_unmatched[i].count);
			out += buf;
		}
		ReleaseSRWLockShared(&g_unmatchedLock);
		return out;
	}

	bool Available() { return g_available.load(); }
	uint64_t Models() { return g_models.load(); }
	uint64_t Lights() { return g_lights.load(); }
	uint64_t Recolored() { return g_recolored.load(); }
}
