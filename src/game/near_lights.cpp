// near_lights.cpp - model light recolouring.
//
// Nearby lamps are lit by CLightAttr entries baked into the prop's drawable
// (or fragment, since most lamp posts are breakable .yft), not by the LOD
// light ymaps. This intercepts the moment the game finishes loading a model
// and rewrites the colour of every light in it that reads as sodium orange,
// with the same matcher and target as the LOD tier.
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
//   - The streaming engine completes a load with `call [vtable+0x68]`
//     (slot 13; args: store, slot index). On the model stores that is one
//     shared 22-byte stub forwarding to slot 43; it is detoured by code with
//     a thunk that passes all registers through and only acts for the three
//     stores of interest.
//   - The placed model is the first field of the store's pool entry
//     (atPoolBase at store+56; name hash at +12). NEVER call vtable slot 8
//     for this: on these stores it writes through a null array.
//   - Layouts (verified live: names read back, lights decode as expected):
//       gtaDrawable  : name ptr +0xA8, lights ptr +0xB0, count u16 +0xB8
//       gtaFragType  : name ptr +0x58, lights +0x110/+0x118, drawable +0x30,
//                      drawable array +0x38 / count u32 +0x48
//       pgDictionary<gtaDrawable>: drawable* array +0x30, count u16 +0x38
//       CLightAttr   : 168 bytes; colour +24..26, flashiness +27,
//                      intensity +28, type +38, volume colour +84..86
#include "game/near_lights.h"
#include "color/recolor.h"
#include "game/track.h"
#include "hook/pattern.h"
#include "plugin/log.h"
#include "plugin/plugin.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace lodlight::nearlights
{
	namespace
	{
		constexpr const char* kManagerPattern = "74 1A 8B 15 ? ? ? ? 48 8D 0D ? ? ? ? 41";
		constexpr ptrdiff_t kManagerOffset = 11;
		constexpr size_t kModuleMgrOffset = 144;   // static_assert in Cfx Streaming.h
		constexpr size_t kManagerScanSpan = 0x1000;
		constexpr size_t kStoreNameOffsetMax = 64;
		constexpr size_t kPoolOffset = 56;
		constexpr int kSlotRemove = 3;
		constexpr int kSlotLoadComplete = 13;

		constexpr size_t kDrawableNamePtr = 0xA8;
		constexpr size_t kDrawableLightsPtr = 0xB0;
		constexpr size_t kDrawableLightsCount = 0xB8;
		constexpr size_t kFragNamePtr = 0x58;
		constexpr size_t kFragLightsPtr = 0x110;
		constexpr size_t kFragLightsCount = 0x118;
		constexpr size_t kFragDrawablePtr = 0x30;
		constexpr size_t kFragDrawableArrayPtr = 0x38;
		constexpr size_t kFragDrawableArrayCount = 0x48;
		constexpr size_t kDictDrawablesPtr = 0x30;
		constexpr size_t kDictDrawablesCount = 0x38;

		constexpr size_t kLightAttrSize = 168;
		constexpr size_t kLightColour = 24;
		constexpr size_t kLightFlashiness = 27;
		constexpr size_t kLightVolumeColour = 84;

		constexpr uint32_t kMaxLightsPerModel = 4096;
		constexpr uint32_t kMaxDrawablesPerContainer = 4096;

		// Real signature beyond (this, idx) unknown; all four argument
		// registers and rax are passed straight through.
		using LoadCompleteFn = void* (*)(void* store, int32_t idx, void* a, void* b);

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

		std::atomic<bool> g_available{ false };
		std::atomic<uint64_t> g_models{ 0 };
		std::atomic<uint64_t> g_lights{ 0 };
		std::atomic<uint64_t> g_recolored{ 0 };
		std::atomic<uint64_t> g_completeCalls{ 0 };
		uintptr_t g_base = 0;
		size_t g_size = 0;

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

		// Committed, readable memory covering [p, p+n)?
		bool Readable(const void* p, size_t n)
		{
			if (!p)
				return false;
			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
				return false;
			if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
				return false;
			uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
			return reinterpret_cast<uintptr_t>(p) + n <= end;
		}

		// Cfx hook::get_address: rel32 at p, relative to p+4.
		uintptr_t Rel32At(uintptr_t p)
		{
			return p + 4 + static_cast<uintptr_t>(static_cast<intptr_t>(*reinterpret_cast<const int32_t*>(p)));
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

		// ------------------------------------------------------------ store discovery

		// A strStreamingModule: vtable in the image, Remove and load-complete
		// slots in the image.
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

		// Find, in the first `span` bytes of the manager, either an inline run
		// of module pointers containing `known`, or an atArray header whose
		// array contains it.
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

		// Field of a strStreamingModule that points at its class name, found
		// by looking for "MapDataStore" in the known ymap store.
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

		// ------------------------------------------------------------ light attributes

		void CollectDrawableLights(const void* drawable, std::vector<uint8_t*>& out)
		{
			if (!PlausiblePtr(drawable) || !Readable(drawable, kDrawableLightsCount + 2))
				return;
			uint8_t* lights = Read<uint8_t*>(drawable, kDrawableLightsPtr);
			uint16_t count = Read<uint16_t>(drawable, kDrawableLightsCount);
			if (!PlausiblePtr(lights) || count == 0 || count > kMaxLightsPerModel)
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
				if (PlausiblePtr(lights) && count > 0 && count <= kMaxLightsPerModel)
					for (uint16_t i = 0; i < count; ++i)
						out.push_back(lights + static_cast<size_t>(i) * kLightAttrSize);

				CollectDrawableLights(Read<void*>(obj, kFragDrawablePtr), out);

				void** arr = Read<void**>(obj, kFragDrawableArrayPtr);
				uint32_t n = Read<uint32_t>(obj, kFragDrawableArrayCount);
				if (PlausiblePtr(arr) && n > 0 && n <= kMaxDrawablesPerContainer && Readable(arr, n * sizeof(void*)))
					for (uint32_t i = 0; i < n; ++i)
						CollectDrawableLights(arr[i], out);
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

		// Recolour one light in place. Returns true if it changed.
		bool RecolourLight(uint8_t* attr, const Config& cfg)
		{
			if (!cfg.nearEnabled || !cfg.match.enabled)
				return false;
			if (attr[kLightFlashiness] != 0) // strobes, flickers: leave alone
				return false;

			bool changed = false;
			for (size_t off : { kLightColour, kLightVolumeColour })
			{
				uint8_t* c = attr + off;
				uint32_t packed = PackBytes(c);
				if (Recolor(packed, cfg.match))
				{
					UnpackBytes(packed, c);
					changed = true;
				}
			}
			return changed;
		}

		// track::RepaintFn: restore originals, then recolour per cfg.
		bool Repaint(track::Kind kind, void* obj, const std::vector<uint32_t>& originals, const Config& cfg, uint64_t& lights, uint64_t& changed)
		{
			std::vector<uint8_t*> attrs;
			CollectLights(kind, obj, attrs);
			if (attrs.size() * 2 != originals.size())
				return false;
			for (size_t i = 0; i < attrs.size(); ++i)
			{
				UnpackBytes(originals[i * 2], attrs[i] + kLightColour);
				UnpackBytes(originals[i * 2 + 1], attrs[i] + kLightVolumeColour);
				if (RecolourLight(attrs[i], cfg))
					changed++;
			}
			lights += attrs.size();
			return true;
		}

		bool RepaintYdr(void* o, const std::vector<uint32_t>& r, const Config& c, uint64_t& l, uint64_t& ch) { return Repaint(track::Ydr, o, r, c, l, ch); }
		bool RepaintYft(void* o, const std::vector<uint32_t>& r, const Config& c, uint64_t& l, uint64_t& ch) { return Repaint(track::Yft, o, r, c, l, ch); }
		bool RepaintYdd(void* o, const std::vector<uint32_t>& r, const Config& c, uint64_t& l, uint64_t& ch) { return Repaint(track::Ydd, o, r, c, l, ch); }

		// ------------------------------------------------------------ the hook

		void OnLoaded(StoreHook& h, uint32_t idx, void* obj)
		{
			if (!obj)
				return;

			std::vector<uint8_t*> attrs;
			CollectLights(h.kind, obj, attrs);
			if (attrs.empty())
				return;

			const Config cfg = GetConfig();
			std::vector<uint32_t> originals;
			originals.reserve(attrs.size() * 2);
			uint32_t changed = 0;
			for (uint8_t* attr : attrs)
			{
				originals.push_back(PackBytes(attr + kLightColour));
				originals.push_back(PackBytes(attr + kLightVolumeColour));
				if (RecolourLight(attr, cfg))
					changed++;
			}

			g_models++;
			g_lights += attrs.size();
			g_recolored += changed;

			if (cfg.nearLog)
			{
				char name[48];
				const uint8_t* c0 = attrs[0] + kLightColour;
				Log("near: %s slot %u '%s' lights=%u recolored=%u light[0] was (%u,%u,%u)",
					h.ext, idx, ModelName(h.kind, obj, name, sizeof(name)), (unsigned)attrs.size(), changed,
					originals[0] >> 16 & 0xFF, originals[0] >> 8 & 0xFF, originals[0] & 0xFF);
				(void)c0;
			}

			if (cfg.liveRepaint)
				track::Register(h.kind, h.store, idx, obj, std::move(originals));
		}

		template <int K>
		void* CompleteThunk(void* store, int32_t idx, void* a, void* b)
		{
			void* r = g_hooks[K].orig(store, idx, a, b);
			const uint64_t n = ++g_completeCalls;
			if (n <= 3)
				Log("near: load-complete #%llu: store=%p idx=%d", (unsigned long long)n, store, idx);
			if (idx >= 0)
			{
				for (StoreHook& h : g_hooks)
				{
					if (h.store && store == h.store)
					{
						OnLoaded(h, static_cast<uint32_t>(idx), PoolObject(h.store, static_cast<uint32_t>(idx)));
						break;
					}
				}
			}
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
	}

	bool Init(uintptr_t imageBase, size_t imageSize, void* knownYmapStore)
	{
		g_base = imageBase;
		g_size = imageSize;

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
			Log("near: no streaming manager candidate holds a module table containing the ymap store; near-light hooks not installed");
			return false;
		}
		Log("near: streaming manager %p (base+0x%llX), %u modules at +0x%X", manager,
			(unsigned long long)(reinterpret_cast<uintptr_t>(manager) - g_base), table.count, (unsigned)table.offset);

		// 2. module names
		const int nameOffset = FindNameOffset(knownYmapStore);
		if (nameOffset < 0)
		{
			Log("near: no field of the ymap store points at \"MapDataStore\"; near-light hooks not installed");
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
				// shared with an earlier store; that thunk matches by store pointer
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

		track::SetRepaint(track::Ydr, &RepaintYdr);
		track::SetRepaint(track::Yft, &RepaintYft);
		track::SetRepaint(track::Ydd, &RepaintYdd);

		g_available = hooked > 0;
		return hooked > 0;
	}

	bool Available() { return g_available.load(); }
	uint64_t Models() { return g_models.load(); }
	uint64_t Lights() { return g_lights.load(); }
	uint64_t Recolored() { return g_recolored.load(); }
}
