// NearLights.cpp - model light recolouring.
//
// Nearby lamps are lit by CLightAttr entries baked into the prop's drawable
// (or fragment, since most lamp posts are breakable .yft), not by the LOD
// light ymaps. This intercepts the moment the game places a freshly
// streamed model and rewrites the colour of every light in it that reads as
// sodium orange, with the same matcher and target as the LOD tier.
//
// How the stores are found, without inventing byte patterns and without
// trusting any undocumented layout:
//   - streaming::Manager singleton: Cfx code/components/gta-streaming-five/
//     src/Streaming.cpp, get_address(get_pattern("74 1A 8B 15 ? ? ? ? 48 8D
//     0D ? ? ? ? 41", 11)); its strStreamingModuleMgr is at +144
//     (static_assert in Cfx's Streaming.h). Every match of that pattern
//     decodes to the same global on b3751.
//   - The module array inside the module manager is located by looking, in
//     the first 64 bytes of the manager, for an atArray whose entries
//     include the ymap store the map hook already receives.
//   - Each module's extension string ("ymap", "ydr", ...) is found by
//     looking, in the known ymap store's first 56 bytes, for a pointer to
//     the text "ymap"; that field offset is then used for every module.
//   - strStreamingModule vtable (GTA V, Cfx Streaming.h order): slot 3
//     Remove, 5 Load, 6 PlaceResource, 8 GetPtr. PlaceResource's *code* is
//     detoured (address read from the vtable, never written), then GetPtr
//     yields the placed object.
//
// Layouts (CodeWalker CodeWalker.Core/GameFiles/Resources/Drawable.cs and
// Frag.cs, gen8 / legacy builds):
//   gtaDrawable    : lights ptr +0xB0, count u16 +0xB8 (DrawableBase = 0xA8)
//   gtaFragType    : lights ptr +0x110, count u16 +0x118; DrawablePointer
//                    +0x30; DrawableArrayPointer +0x38, count u32 +0x48
//   pgDictionary<gtaDrawable> (ydd): Drawable* array +0x30, count u16 +0x38
//   CLightAttr     : 168 bytes; colour bytes +24..26, flashiness +27,
//                    intensity +28, flags +32, type +38, volume outer colour
//                    +84..86
#include "NearLights.h"
#include "Log.h"
#include "Pattern.h"
#include "Recolor.h"
#include "Shared.h"
#include "Track.h"

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
		constexpr size_t kModuleMgrOffset = 144;
		constexpr const char* kGetModulePattern = "74 15 48 8D 50 01 48 8D";
		constexpr ptrdiff_t kGetModuleOffset = 13;

		// The hook point is fwAssetStore<T>::FinishLoading(idx, T** obj): the
		// per-store override of the virtual that Cfx hooks for the ymap store
		// (by pattern). Its slot is discovered by finding that known address
		// in the ymap store's vtable. Cfx Streaming.h slots used for sanity:
		// 3 Remove, 8 GetPtr.
		constexpr int kSlotGetPtr = 8;
		constexpr int kSlotPlaceResource = 6;
		// Observed on b3751: the streaming engine completes a load with
		// `call [vtable+0x68]` (slot 13, args: this, idx); the ymap version of
		// that function is what calls fwMapDataStore::FinishLoading. Hooking
		// the same slot on the model stores catches their loads.
		constexpr int kSlotLoadComplete = 13;
		constexpr int kMaxVtableSlots = 48;
		// Diagnostic: observe only, never write to model memory, never call
		// GetPtr for real use.
		constexpr bool kDryRun = false;

		constexpr size_t kDrawableLightsPtr = 0xB0;
		constexpr size_t kDrawableLightsCount = 0xB8;
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

		using GetModuleFn = void* (*)(void* moduleMgr, const char* ext);
		using FinishLoadingFn = void (*)(void* store, int32_t idx, void** obj);
		// Real signature unknown beyond (this, idx); all four argument registers
		// and rax are passed straight through.
		using LoadCompleteFn = void* (*)(void* store, int32_t idx, void* a, void* b);
		using PlaceFn = void (*)(void* store, uint32_t object, void* blockMap, const char* name);
		using GetPtrFn = void* (*)(void* store, uint32_t object);

		struct StoreHook
		{
			track::Kind kind;
			const char* ext;
			const char* className; // RAGE store name, as found at the module's name field
			void* store = nullptr;
			FinishLoadingFn orig = nullptr;
			PlaceFn origPlace = nullptr;
			LoadCompleteFn origComplete = nullptr;
			bool hooked = false;
			std::atomic<uint64_t> placed{ 0 };
			std::atomic<uint64_t> placeCalls{ 0 };
		};
		StoreHook g_hooks[3] = {
			{ track::Ydr, "ydr", "DrawableStore" },
			{ track::Yft, "yft", "FragmentStore" },
			{ track::Ydd, "ydd", "DwdStore" },
		};

		bool g_getModuleIndirect = false; // GetStreamingModule returns strStreamingModule** on this build
		std::atomic<bool> g_available{ false };
		std::atomic<uint64_t> g_models{ 0 };
		std::atomic<uint64_t> g_lights{ 0 };
		std::atomic<uint64_t> g_recolored{ 0 };
		uintptr_t g_base = 0;
		size_t g_size = 0;
		bool Readable(const void* p, size_t n); // defined below
		uintptr_t CallTarget(uintptr_t p);        // defined below
		std::string DumpQwords(const void* base, size_t bytes); // defined below

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

		// Very cheap plausibility check for a heap pointer: non-null, 8-aligned,
		// user-mode.
		bool PlausiblePtr(const void* p)
		{
			uintptr_t a = reinterpret_cast<uintptr_t>(p);
			return a != 0 && (a & 7) == 0 && a > 0x10000 && a < 0x00007FFFFFFFFFFFull;
		}

		void CollectDrawableLights(const void* drawable, std::vector<uint8_t*>& out)
		{
			if (!PlausiblePtr(drawable))
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
				if (PlausiblePtr(arr) && n > 0 && n <= kMaxDrawablesPerContainer)
					for (uint32_t i = 0; i < n; ++i)
						CollectDrawableLights(arr[i], out);
				break;
			}
			case track::Ydd:
			{
				void** arr = Read<void**>(obj, kDictDrawablesPtr);
				uint16_t n = Read<uint16_t>(obj, kDictDrawablesCount);
				if (PlausiblePtr(arr) && n > 0 && n <= kMaxDrawablesPerContainer)
					for (uint16_t i = 0; i < n; ++i)
						CollectDrawableLights(arr[i], out);
				break;
			}
			default:
				break;
			}
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

		// Restore originals, then recolour per cfg.
		bool Repaint(void* obj, const std::vector<uint32_t>& originals, const Config& cfg, uint64_t& lights, uint64_t& changed, track::Kind kind)
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

		bool RepaintYdr(void* o, const std::vector<uint32_t>& orig, const Config& c, uint64_t& l, uint64_t& ch) { return Repaint(o, orig, c, l, ch, track::Ydr); }
		bool RepaintYft(void* o, const std::vector<uint32_t>& orig, const Config& c, uint64_t& l, uint64_t& ch) { return Repaint(o, orig, c, l, ch, track::Yft); }
		bool RepaintYdd(void* o, const std::vector<uint32_t>& orig, const Config& c, uint64_t& l, uint64_t& ch) { return Repaint(o, orig, c, l, ch, track::Ydd); }

		void OnLoaded(StoreHook& h, uint32_t object, void* obj)
		{
			const uint64_t n = ++h.placed;

			std::vector<uint8_t*> attrs;
			if (PlausiblePtr(obj))
				CollectLights(h.kind, obj, attrs);

			if (n <= 10)
				Log("near: %s FinishLoading: slot %u obj=%p lights=%u", h.ext, object, obj, (unsigned)attrs.size());
			if (!PlausiblePtr(obj) || attrs.empty())
				return;

			const Config cfg = GetConfig();
			std::vector<uint32_t> originals;
			originals.reserve(attrs.size() * 2);
			uint32_t changed = 0;
			uint8_t first[3] = { 0, 0, 0 };
			for (size_t i = 0; i < attrs.size(); ++i)
			{
				if (i == 0)
				{
					first[0] = attrs[i][kLightColour];
					first[1] = attrs[i][kLightColour + 1];
					first[2] = attrs[i][kLightColour + 2];
				}
				originals.push_back(PackBytes(attrs[i] + kLightColour));
				originals.push_back(PackBytes(attrs[i] + kLightVolumeColour));
				if (RecolourLight(attrs[i], cfg))
					changed++;
			}

			g_models++;
			g_lights += attrs.size();
			g_recolored += changed;

			if (cfg.nearLog)
			{
				const char* namePtr = nullptr;
				if (h.kind == track::Ydr)
					namePtr = *reinterpret_cast<const char* const*>(static_cast<const char*>(obj) + 0xA8);
				else if (h.kind == track::Yft)
					namePtr = *reinterpret_cast<const char* const*>(static_cast<const char*>(obj) + 0x58);
				char name[48] = "?";
				if (namePtr && Readable(namePtr, 40))
				{
					int i = 0;
					for (; i < 40 && namePtr[i]; ++i)
						name[i] = (namePtr[i] >= 32 && namePtr[i] < 127) ? namePtr[i] : '.';
					name[i] = 0;
				}
				Log("near: %s slot %u '%s' lights=%u recolored=%u first=(%u,%u,%u)",
					h.ext, object, name, (unsigned)attrs.size(), changed, first[0], first[1], first[2]);
			}

			if (cfg.liveRepaint)
				track::Register(h.kind, h.store, object, obj, std::move(originals));
		}

		template <int K>
		void FinishThunk(void* store, int32_t idx, void** obj)
		{
			g_hooks[K].orig(store, idx, obj);
			if (g_hooks[K].store && store == g_hooks[K].store && idx >= 0)
				OnLoaded(g_hooks[K], static_cast<uint32_t>(idx), obj ? *obj : nullptr);
		}

		// PlaceResource fallback. Logs the first calls unconditionally (with the
		// `this` actually passed) so a this-pointer mismatch is visible.
		template <int K>
		void PlaceThunk(void* store, uint32_t object, void* blockMap, const char* name)
		{
			g_hooks[K].origPlace(store, object, blockMap, name);
			StoreHook& h = g_hooks[K];
			const uint64_t n = ++h.placeCalls;
			if (n <= 10)
			{
				Log("near: %s PlaceResource call #%llu: this=%p (table %p) object=%u name=%s", h.ext, (unsigned long long)n,
					store, h.store, object, (name && Readable(name, 1)) ? name : "?");
			}
			if (h.store && store == h.store)
			{
				void** vt = *static_cast<void***>(h.store);
				void* obj = reinterpret_cast<GetPtrFn>(vt[kSlotGetPtr])(h.store, object);
				OnLoaded(h, object, obj);
			}
		}

		void* PlaceThunkFor(int k)
		{
			switch (k)
			{
			case 0: return reinterpret_cast<void*>(&PlaceThunk<0>);
			case 1: return reinterpret_cast<void*>(&PlaceThunk<1>);
			default: return reinterpret_cast<void*>(&PlaceThunk<2>);
			}
		}

		// The placed object for slot `idx` of a model store: pool entry +0
		// (verified live on b3751: a page-aligned resource block whose vtable is
		// in the game image and whose name pointer reads e.g.
		// "pack:/prop_streetlight_01"). Read-only. Never use vtable[8] for
		// this: on these stores it writes.
		void* PoolObject(void* store, uint32_t idx)
		{
			const char* pool = static_cast<const char*>(store) + 56;
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

		std::atomic<uint64_t> g_completeCalls{ 0 };

		template <int K>
		void* CompleteThunk(void* store, int32_t idx, void* a, void* b)
		{
			void* r = g_hooks[K].origComplete(store, idx, a, b);
			const uint64_t n = ++g_completeCalls;
			if (n <= 5)
				Log("near: load-complete #%llu via %s hook: store=%p idx=%d", (unsigned long long)n, g_hooks[K].ext, store, idx);
			if (idx >= 0)
			{
				for (int k = 0; k < 3; ++k)
				{
					StoreHook& h = g_hooks[k];
					if (h.store && store == h.store)
					{
						if (kDryRun)
						{
							// Read-only survey. Object = pool entry +0 (a page-aligned
							// resource block). Name strings validate the layout:
							// gtaDrawable NamePointer +0xA8, gtaFragType NamePointer +0x58.
							static std::atomic<uint64_t> seen[3];
							static std::atomic<uint64_t> withLights[3];
							static std::atomic<uint64_t> loggedLit[3];
							const uint64_t m = ++h.placed;
							seen[k]++;

							const char* pool = static_cast<const char*>(h.store) + 56;
							char* data = *reinterpret_cast<char* const*>(pool);
							const int8_t* flags = *reinterpret_cast<const int8_t* const*>(pool + 8);
							uint32_t count = *reinterpret_cast<const uint32_t*>(pool + 16);
							uint32_t esz = *reinterpret_cast<const uint32_t*>(pool + 20);
							void* obj = nullptr;
							uint32_t nameHash = 0;
							if (data && flags && static_cast<uint32_t>(idx) < count && esz >= 16 && Readable(flags + idx, 1) && flags[idx] >= 0)
							{
								const char* entry = data + static_cast<size_t>(idx) * esz;
								if (Readable(entry, esz))
								{
									obj = *reinterpret_cast<void* const*>(entry);
									nameHash = *reinterpret_cast<const uint32_t*>(entry + 12);
								}
							}
							if (!PlausiblePtr(obj) || !Readable(obj, 0x140))
								break;

							void* objVt = *static_cast<void* const*>(obj);
							const char* namePtr = nullptr;
							if (h.kind == track::Ydr)
								namePtr = *reinterpret_cast<const char* const*>(static_cast<const char*>(obj) + 0xA8);
							else if (h.kind == track::Yft)
								namePtr = *reinterpret_cast<const char* const*>(static_cast<const char*>(obj) + 0x58);
							char name[48] = "?";
							if (namePtr && PlausiblePtr(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(namePtr) & ~7ull)) && Readable(namePtr, 40))
							{
								int i = 0;
								for (; i < 40 && namePtr[i]; ++i)
									name[i] = (namePtr[i] >= 32 && namePtr[i] < 127) ? namePtr[i] : '.';
								name[i] = 0;
							}

							std::vector<uint8_t*> attrs;
							CollectLights(h.kind, obj, attrs);
							if (!attrs.empty())
								withLights[k]++;

							const bool logIt = (m <= 6) || (!attrs.empty() && loggedLit[k] < 24);
							if (logIt)
							{
								if (!attrs.empty())
									loggedLit[k]++;
								Log("near[dry]: %s idx=%u name=%08X '%s' obj=%p vt=%s lights=%u  (seen %llu, with lights %llu)",
									h.ext, (unsigned)idx, nameHash, name, obj, InImage(objVt) ? "in-image" : "NOT in image",
									(unsigned)attrs.size(), (unsigned long long)seen[k].load(), (unsigned long long)withLights[k].load());
								if (!attrs.empty())
								{
									const uint8_t* c = attrs[0] + kLightColour;
									Log("near[dry]:   light[0]: rgb=(%u,%u,%u) flashiness=%u intensity=%.2f type=%u",
										c[0], c[1], c[2], attrs[0][kLightFlashiness], *reinterpret_cast<const float*>(attrs[0] + 28), attrs[0][38]);
								}
							}
							break;
						}
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

		void* FinishThunkFor(int k)
		{
			switch (k)
			{
			case 0: return reinterpret_cast<void*>(&FinishThunk<0>);
			case 1: return reinterpret_cast<void*>(&FinishThunk<1>);
			default: return reinterpret_cast<void*>(&FinishThunk<2>);
			}
		}

		int FindVtableSlot(const void* store, const void* fn)
		{
			void* const* vt = *static_cast<void* const* const*>(store);
			if (!Readable(vt, sizeof(void*) * kMaxVtableSlots))
				return -1;
			for (int i = 0; i < kMaxVtableSlots; ++i)
				if (vt[i] == fn)
					return i;
			return -1;
		}

		std::string DumpVtable(const void* store)
		{
			std::string out;
			char buf[40];
			void* const* vt = *static_cast<void* const* const*>(store);
			if (!Readable(vt, sizeof(void*) * kMaxVtableSlots))
				return out;
			for (int i = 0; i < kMaxVtableSlots; ++i)
			{
				uintptr_t a = reinterpret_cast<uintptr_t>(vt[i]);
				if (!InImage(vt[i]))
					break;
				snprintf(buf, sizeof(buf), " [%d]=+%llX", i, (unsigned long long)(a - g_base));
				out += buf;
			}
			return out;
		}

		// --- sibling FinishLoading hooks ------------------------------------
		// fwAssetStore<T>::FinishLoading(idx, T** obj) is non-virtual, one
		// copy per store type, all called from the same template code. Every
		// copy found via the call-site pattern is detoured with one generic
		// thunk that matches `store` against the stores we care about.
		constexpr int kMaxSiblings = 32;
		struct Sibling
		{
			void* target = nullptr;
			FinishLoadingFn orig = nullptr;
		};
		Sibling g_siblings[kMaxSiblings];
		int g_numSiblings = 0;
		std::atomic<uint64_t> g_siblingCalls{ 0 };

		template <int I>
		void SiblingThunk(void* store, int32_t idx, void** obj)
		{
			g_siblings[I].orig(store, idx, obj);
			const uint64_t n = ++g_siblingCalls;
			if (n <= 5)
				Log("near: sibling FinishLoading #%llu: store=%p idx=%d obj=%p", (unsigned long long)n, store, idx, obj ? *obj : nullptr);
			if (idx < 0)
				return;
			for (int k = 0; k < 3; ++k)
			{
				if (g_hooks[k].store && store == g_hooks[k].store)
				{
					OnLoaded(g_hooks[k], static_cast<uint32_t>(idx), obj ? *obj : nullptr);
					break;
				}
			}
		}

		void* SiblingThunkFor(int i)
		{
			switch (i)
			{
			case 0: return reinterpret_cast<void*>(&SiblingThunk<0>);
			case 1: return reinterpret_cast<void*>(&SiblingThunk<1>);
			case 2: return reinterpret_cast<void*>(&SiblingThunk<2>);
			case 3: return reinterpret_cast<void*>(&SiblingThunk<3>);
			case 4: return reinterpret_cast<void*>(&SiblingThunk<4>);
			case 5: return reinterpret_cast<void*>(&SiblingThunk<5>);
			case 6: return reinterpret_cast<void*>(&SiblingThunk<6>);
			case 7: return reinterpret_cast<void*>(&SiblingThunk<7>);
			case 8: return reinterpret_cast<void*>(&SiblingThunk<8>);
			case 9: return reinterpret_cast<void*>(&SiblingThunk<9>);
			case 10: return reinterpret_cast<void*>(&SiblingThunk<10>);
			case 11: return reinterpret_cast<void*>(&SiblingThunk<11>);
			case 12: return reinterpret_cast<void*>(&SiblingThunk<12>);
			case 13: return reinterpret_cast<void*>(&SiblingThunk<13>);
			case 14: return reinterpret_cast<void*>(&SiblingThunk<14>);
			case 15: return reinterpret_cast<void*>(&SiblingThunk<15>);
			case 16: return reinterpret_cast<void*>(&SiblingThunk<16>);
			case 17: return reinterpret_cast<void*>(&SiblingThunk<17>);
			case 18: return reinterpret_cast<void*>(&SiblingThunk<18>);
			case 19: return reinterpret_cast<void*>(&SiblingThunk<19>);
			case 20: return reinterpret_cast<void*>(&SiblingThunk<20>);
			case 21: return reinterpret_cast<void*>(&SiblingThunk<21>);
			case 22: return reinterpret_cast<void*>(&SiblingThunk<22>);
			case 23: return reinterpret_cast<void*>(&SiblingThunk<23>);
			case 24: return reinterpret_cast<void*>(&SiblingThunk<24>);
			case 25: return reinterpret_cast<void*>(&SiblingThunk<25>);
			case 26: return reinterpret_cast<void*>(&SiblingThunk<26>);
			case 27: return reinterpret_cast<void*>(&SiblingThunk<27>);
			case 28: return reinterpret_cast<void*>(&SiblingThunk<28>);
			case 29: return reinterpret_cast<void*>(&SiblingThunk<29>);
			case 30: return reinterpret_cast<void*>(&SiblingThunk<30>);
			case 31: return reinterpret_cast<void*>(&SiblingThunk<31>);
			default: return nullptr;
			}
		}

		std::string HexPattern(const uint8_t* p, size_t n)
		{
			std::string out;
			char buf[8];
			for (size_t i = 0; i < n; ++i)
			{
				snprintf(buf, sizeof(buf), "%s%02X", i ? " " : "", p[i]);
				out += buf;
			}
			return out;
		}

		// Find every call site whose surrounding bytes match the known one,
		// and collect the distinct call targets. Tries shrinking windows
		// before the call, then after it, until a sane number of siblings
		// (2..kMaxSiblings) appears.
		int DiscoverSiblings(uintptr_t callSiteReturn, void* knownFinish, std::vector<void*>& targets)
		{
			const uintptr_t call = callSiteReturn - 5;
			if (!Readable(reinterpret_cast<void*>(call - 64), 64 + 5 + 64))
				return 0;
			if (*reinterpret_cast<const uint8_t*>(call) != 0xE8 || CallTarget(call) != reinterpret_cast<uintptr_t>(knownFinish))
			{
				Log("near: instruction before the FinishLoading return address is not a call to it (byte %02X, target base+0x%llX)",
					*reinterpret_cast<const uint8_t*>(call), (unsigned long long)(CallTarget(call) - g_base));
				return 0;
			}

			const size_t windows[] = { 24, 16, 12, 8 };
			for (int mode = 0; mode < 2; ++mode) // 0: bytes before the call, 1: bytes after it
			{
				for (size_t w : windows)
				{
					std::string text;
					if (mode == 0)
						text = HexPattern(reinterpret_cast<const uint8_t*>(call - w), w) + " E8";
					else
						text = std::string("E8 ? ? ? ? ") + HexPattern(reinterpret_cast<const uint8_t*>(call + 5), w);

					Pattern pat;
					if (!ParsePattern(text, pat))
						continue;
					auto hits = FindPattern(pat, g_base, g_size, 128);
					targets.clear();
					for (uintptr_t h : hits)
					{
						uintptr_t site = (mode == 0) ? h + w : h;
						uintptr_t t = CallTarget(site);
						if (!InImage(reinterpret_cast<void*>(t)))
							continue;
						bool dup = false;
						for (void* x : targets)
							if (x == reinterpret_cast<void*>(t))
								dup = true;
						if (!dup)
							targets.push_back(reinterpret_cast<void*>(t));
					}
					Log("near: call-site pattern (%s, %u bytes) -> %u sites, %u distinct targets",
						mode == 0 ? "before" : "after", (unsigned)w, (unsigned)hits.size(), (unsigned)targets.size());
					if (targets.size() >= 2 && targets.size() <= static_cast<size_t>(kMaxSiblings))
						return static_cast<int>(targets.size());
				}
			}
			return 0;
		}

		// All matches of a pattern (a few at most), in image order. Cfx takes
		// the first; on b3751 the manager pattern has a false first match, so
		// every candidate is validated against the known ymap store instead.
		std::vector<uintptr_t> FindAll(const char* text, ptrdiff_t offset)
		{
			std::vector<uintptr_t> out;
			Pattern pat;
			if (!ParsePattern(text, pat))
				return out;
			for (uintptr_t h : FindPattern(pat, g_base, g_size, 8))
				out.push_back(h + offset);
			if (out.empty())
				Log("near: pattern '%s' not found", text);
			return out;
		}

		// Does `p` look like a strStreamingModule: vtable in the image, and
		// the slots we use in the image too.
		bool LooksLikeStore(const void* p)
		{
			if (!PlausiblePtr(p))
				return false;
			void* const* vt = *static_cast<void* const* const*>(p);
			if (!InImage(vt))
				return false;
			return InImage(vt[3]) && InImage(vt[kSlotGetPtr]);
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

		struct ModuleArray
		{
			void** modules = nullptr;
			uint32_t count = 0;
			size_t offset = 0; // within the module manager
		};

		// Find, anywhere in the first `span` bytes of `region`, a pointer to
		// an array that contains `known` (atArray header: ptr, u16 count) OR
		// `known` stored inline. Only dereferences after Readable() checks.
		bool FindModuleArray(const char* region, size_t span, void* known, ModuleArray& out)
		{
			if (!Readable(region, span))
				return false;
			for (size_t off = 0; off + 16 <= span; off += 8)
			{
				void** arr = *reinterpret_cast<void** const*>(region + off);
				if (arr == known)
				{
					// inline table of module pointers: treat the run as the array
					void** start = reinterpret_cast<void**>(const_cast<char*>(region + off));
					// walk back to the first plausible store, forward to the last
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
				if (!PlausiblePtr(arr) || count < 2 || count > 256)
					continue;
				if (!Readable(arr, static_cast<size_t>(count) * sizeof(void*)))
					continue;
				for (uint32_t i = 0; i < count; ++i)
				{
					if (arr[i] == known)
					{
						out.modules = arr;
						out.count = count;
						out.offset = off;
						return true;
					}
				}
			}
			return false;
		}

		std::string DumpQwords(const void* base, size_t bytes)
		{
			std::string out;
			char buf[48];
			const char* b = static_cast<const char*>(base);
			for (size_t off = 0; off + 8 <= bytes; off += 8)
			{
				snprintf(buf, sizeof(buf), " +%X=%016llX", (unsigned)off, *reinterpret_cast<const unsigned long long*>(b + off));
				out += buf;
			}
			return out;
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

		// Find the field of a strStreamingModule that points at its name.
		// On b3751 the name is the RAGE class-style name ("MapDataStore");
		// older/other builds may carry the extension ("ymap"). Sets
		// `classStyle` accordingly. Returns -1 if not found.
		int FindNameOffset(const void* knownYmap, bool& classStyle)
		{
			const char* base = static_cast<const char*>(knownYmap);
			if (!Readable(base, 64))
				return -1;
			for (size_t off = 8; off + 8 <= 64; off += 8)
			{
				const char* str = *reinterpret_cast<const char* const*>(base + off);
				if (!PlausiblePtr(str) && !InImage(str))
					continue;
				if (!Readable(str, 16))
					continue;
				if (StartsWith(str, "MapDataStore"))
				{
					classStyle = true;
					return static_cast<int>(off);
				}
				if (StartsWith(str, "ymap") && str[4] == '\0')
				{
					classStyle = false;
					return static_cast<int>(off);
				}
			}
			return -1;
		}

		const char* ModuleName(const void* module, int nameOffset)
		{
			const char* str = *reinterpret_cast<const char* const*>(static_cast<const char*>(module) + nameOffset);
			if (!PlausiblePtr(str) && !InImage(str))
				return nullptr;
			if (!Readable(str, 32))
				return nullptr;
			return str;
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

		bool InArray(const ModuleArray& arr, const void* m)
		{
			for (uint32_t i = 0; i < arr.count; ++i)
				if (arr.modules[i] == m)
					return true;
			return false;
		}

		// strStreamingModuleMgr::GetStreamingModule(ext), from Cfx's pattern,
		// accepted only if it returns the known ymap store for "ymap".
		GetModuleFn FindGetModuleFn(void* moduleMgr, void* knownYmap)
		{
			for (uintptr_t site : FindAll(kGetModulePattern, kGetModuleOffset))
			{
				if (*reinterpret_cast<const uint8_t*>(site) != 0xE8)
				{
					Log("near: GetStreamingModule site base+0x%llX is not a call; skipping", (unsigned long long)(site - g_base));
					continue;
				}
				auto fn = reinterpret_cast<GetModuleFn>(CallTarget(site));
				if (!InImage(reinterpret_cast<void*>(fn)))
					continue;
				void* r = fn(moduleMgr, "ymap");
				bool direct = (r == knownYmap);
				bool indirect = !direct && PlausiblePtr(r) && Readable(r, 8) && *static_cast<void**>(r) == knownYmap;
				Log("near: GetStreamingModule candidate base+0x%llX: (\"ymap\") -> %p, known %p%s",
					(unsigned long long)(reinterpret_cast<uintptr_t>(fn) - g_base), r, knownYmap,
					direct ? " OK" : (indirect ? " OK (returns pointer-to-module)" : ""));
				if (direct || indirect)
				{
					g_getModuleIndirect = indirect;
					return fn;
				}
			}
			return nullptr;
		}

		std::string DumpPointerFields(const void* store)
		{
			std::string out;
			const char* base = static_cast<const char*>(store);
			char buf[128];
			for (size_t off = 8; off + 8 <= 64; off += 8)
			{
				const char* ptr = *reinterpret_cast<const char* const*>(base + off);
				if (!PlausiblePtr(ptr) || !Readable(ptr, 16))
					continue;
				char txt[17];
				for (int i = 0; i < 16; ++i)
					txt[i] = (ptr[i] >= 32 && ptr[i] < 127) ? ptr[i] : '.';
				txt[16] = 0;
				snprintf(buf, sizeof(buf), " +%u->\"%s\"", (unsigned)off, txt);
				out += buf;
			}
			return out;
		}

	}

	bool Init(uintptr_t imageBase, size_t imageSize, void* knownYmapStore, void* knownFinishLoading, uintptr_t callSiteReturn)
	{
		g_base = imageBase;
		g_size = imageSize;

		if (!LooksLikeStore(knownYmapStore))
		{
			Log("near: the ymap store %p from the map hook does not look like a streaming module; giving up", knownYmapStore);
			return false;
		}

		Log("near: ymap store %p fields:%s", knownYmapStore, DumpQwords(knownYmapStore, 64).c_str());
		Log("near: ymap store pointer fields as text:%s", DumpPointerFields(knownYmapStore).c_str());

		ModuleArray arr;
		void* moduleMgr = nullptr;
		void* manager = nullptr;
		for (uintptr_t site : FindAll(kManagerPattern, kManagerOffset))
		{
			void* mgr = reinterpret_cast<void*>(Rel32At(site));
			if (!InImage(mgr))
				continue;
			if (!manager)
			{
				manager = mgr;
				Log("near: streaming manager %p (base+0x%llX); moduleMgr region:%s", mgr,
					(unsigned long long)(reinterpret_cast<uintptr_t>(mgr) - g_base),
					DumpQwords(static_cast<char*>(mgr) + kModuleMgrOffset, 96).c_str());
			}
			if (!moduleMgr && FindModuleArray(static_cast<const char*>(mgr), 0x1000, knownYmapStore, arr))
			{
				Log("near: module table found at manager+0x%X: %u modules", (unsigned)arr.offset, arr.count);
				moduleMgr = static_cast<char*>(mgr) + kModuleMgrOffset;
			}
			if (moduleMgr)
				break;
		}
		if (!manager)
		{
			Log("near: no streaming manager candidate inside the game image; near-light hooks not installed");
			return false;
		}
		if (!moduleMgr)
		{
			Log("near: no module table containing the ymap store within the manager; trying the lookup function anyway");
			moduleMgr = static_cast<char*>(manager) + kModuleMgrOffset;
		}

		if (!arr.modules)
		{
			Log("near: no module table containing the ymap store within the manager; near-light hooks not installed");
			return false;
		}

		bool classStyle = false;
		const int nameOffset = FindNameOffset(knownYmapStore, classStyle);
		if (nameOffset < 0)
		{
			Log("near: no field of the ymap store names it; near-light hooks not installed");
			return false;
		}
		Log("near: module name field at store+%d (%s)", nameOffset, classStyle ? "class names" : "extensions");

		{
			std::string names;
			for (uint32_t i = 0; i < arr.count; ++i)
			{
				const char* n = LooksLikeStore(arr.modules[i]) ? ModuleName(arr.modules[i], nameOffset) : nullptr;
				names += ' ';
				names += n ? n : "?";
			}
			Log("near: modules:%s", names.c_str());
		}

		auto resolve = [&](const StoreHook& h) -> void* {
			const char* want = classStyle ? h.className : h.ext;
			for (uint32_t i = 0; i < arr.count; ++i)
			{
				void* m = arr.modules[i];
				if (LooksLikeStore(m) && NameIs(ModuleName(m, nameOffset), want))
					return m;
			}
			return nullptr;
		};

		int resolved = 0;
		for (int k = 0; k < 3; ++k)
		{
			StoreHook& h = g_hooks[k];
			h.store = resolve(h);
			if (!h.store)
			{
				Log("near: no '%s' (%s) module found", h.ext, h.className);
				continue;
			}
			Log("near: '%s' store is %p, vtable:%s", h.ext, h.store, DumpVtable(h.store).c_str());
			resolved++;
		}
		if (resolved == 0)
			return false;

		(void)callSiteReturn;
		(void)knownFinishLoading;

		int hooked = 0;
		for (int k = 0; k < 3; ++k)
		{
			StoreHook& h = g_hooks[k];
			if (!h.store)
				continue;
			void** vt = *static_cast<void***>(h.store);
			if (!Readable(vt, sizeof(void*) * (kSlotLoadComplete + 1)))
				continue;
			void* fn = vt[kSlotLoadComplete];
			if (!InImage(fn))
			{
				Log("near: '%s' vtable[%d] = %p is outside the game image; skipping", h.ext, kSlotLoadComplete, fn);
				h.store = nullptr;
				continue;
			}
			{
				const uint8_t* b = static_cast<const uint8_t*>(fn);
				char hex[80] = "";
				size_t pp = 0;
				for (int j = 0; j < 24; ++j)
					pp += static_cast<size_t>(snprintf(hex + pp, sizeof(hex) - pp, "%02X ", b[j]));
				Log("near: '%s' vtable[%d] at base+0x%llX bytes: %s", h.ext, kSlotLoadComplete, (unsigned long long)(reinterpret_cast<uintptr_t>(fn) - g_base), hex);
			}
			MH_STATUS st = MH_CreateHook(fn, CompleteThunkFor(k), reinterpret_cast<void**>(&h.origComplete));
			if (st == MH_ERROR_ALREADY_CREATED)
			{
				// shared with an earlier store; that thunk matches by store pointer
				h.hooked = true;
				hooked++;
				Log("near: '%s' vtable[%d] at %p already detoured (shared)", h.ext, kSlotLoadComplete, fn);
				continue;
			}
			if (st == MH_OK)
				st = MH_EnableHook(fn);
			if (st != MH_OK)
			{
				Log("near: detouring '%s' vtable[%d] at %p failed: %s", h.ext, kSlotLoadComplete, fn, MH_StatusToString(st));
				h.origComplete = nullptr;
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

	bool Available()
	{
		return g_available.load();
	}

	uint64_t Models() { return g_models.load(); }
	uint64_t Lights() { return g_lights.load(); }
	uint64_t Recolored() { return g_recolored.load(); }
}
