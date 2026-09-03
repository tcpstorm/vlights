// Main.cpp - DllMain, the map-data (LOD light) detour, hotkeys, and the
// core config/repaint interface used by the menu.
//
// Load order in FiveM: the asi-five component LoadLibrary()s every .asi in
// FiveM.app/plugins/ from its DoGameLoad step, on the main thread, unless
// the server's sv_pureLevel is >= 2 (code/components/asi-five/src/Component.cpp).
// That is after the game image is mapped and before the world streams in.
//
// The map hook is installed synchronously inside DllMain, on purpose. The
// main thread is the only caller of fwMapDataStore::FinishLoading, and while
// we are in DllMain it is blocked inside LoadLibrary, so the target cannot
// be executing while its first bytes are overwritten. Every other detour
// (store Remove, model PlaceResource) is installed from inside that hook,
// i.e. from the main thread that is their only caller. That is why MinHook
// never needs to suspend threads here (see third_party/minhook/src/hook.c;
// FiveM stubs out the Toolhelp snapshot anyway).
//
// Cfx has already detoured FinishLoading by then; MinHook detours the
// detour, so our hook runs first and Cfx's numStreetLights fix-up runs after
// us. We never rely on that fix-up.
#include "Config.h"
#include "GameStructs.h"
#include "Log.h"
#include "NearLights.h"
#include "Pattern.h"
#include "Recolor.h"
#include "Shared.h"
#include "Track.h"

#include <windows.h>
#include <tlhelp32.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define LODLIGHT_VERSION "0.7.2"

extern "C" int mh_lodlight_freeze_method;  // from the patched MinHook
extern "C" int mh_lodlight_allow_suspend;  // 0 = never SuspendThread (default)

namespace
{
	HMODULE g_module = nullptr;
	std::wstring g_configPath;

	SRWLOCK g_configLock = SRWLOCK_INIT;
	lodlight::Config g_config;

	FinishLoadingFn g_origFinishLoading = nullptr;
	void* g_finishLoadingTarget = nullptr;
	uintptr_t g_imageBase = 0;
	size_t g_imageSize = 0;
	bool g_nearInitTried = false;

	std::atomic<int> g_samplesLogged{ 0 };
	std::atomic<uint64_t> g_calls{ 0 };
	std::atomic<uint64_t> g_blocks{ 0 };
	std::atomic<uint64_t> g_lights{ 0 };
	std::atomic<uint64_t> g_recolored{ 0 };
	std::atomic<uint64_t> g_lastRepaintLights{ 0 };
	std::atomic<uint64_t> g_lastRepaintRecolored{ 0 };

	std::wstring PluginDir()
	{
		wchar_t buf[MAX_PATH];
		DWORD n = GetModuleFileNameW(g_module, buf, MAX_PATH);
		std::wstring path(buf, n);
		size_t slash = path.find_last_of(L"\\/");
		return (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
	}

	void LogConfig(const lodlight::Config& cfg, const char* why)
	{
		lodlight::Log("config (%s): enabled=%d source=(%.0f,%.0f,%.0f) hue=%.1f window=%.1f min_sat=%.2f target=(%.0f,%.0f,%.0f) blend=%.2f keep_brightness=%d near=%d near_log=%d log_samples=%d log_blocks=%d reload_key=0x%X menu_key=0x%X live_repaint=%d",
			why, cfg.match.enabled, cfg.source.r, cfg.source.g, cfg.source.b, cfg.match.sourceHue, cfg.match.hueWindow,
			cfg.match.minSaturation, cfg.match.target.r, cfg.match.target.g, cfg.match.target.b, cfg.match.blend,
			cfg.match.keepBrightness, cfg.nearEnabled, cfg.nearLog, cfg.logSamples, cfg.logBlocks, cfg.reloadKey, cfg.menuKey, cfg.liveRepaint);
	}

	// ------------------------------------------------------------------ LOD lights

	// track::RepaintFn for ymap blocks.
	bool RepaintMapData(void* obj, const std::vector<uint32_t>& originals, const lodlight::Config& cfg, uint64_t& lights, uint64_t& changed)
	{
		CMapData* md = static_cast<CMapData*>(obj);
		CDistantLODLight& dl = md->distantLodLights;
		if (dl.rgbi.data == nullptr || dl.rgbi.count != originals.size())
			return false;
		for (size_t i = 0; i < originals.size(); ++i)
		{
			uint32_t v = originals[i];
			if (lodlight::Recolor(v, cfg.match))
				changed++;
			dl.rgbi.data[i] = v;
		}
		lights += originals.size();
		return true;
	}

	void ProcessMapData(void* store, int32_t idx, CMapData* md)
	{
		const lodlight::Config cfg = lodlight::GetConfig();
		CDistantLODLight& dl = md->distantLodLights;

		// The first calls are logged regardless of content so "hook never
		// fires" and "every block was empty" are distinguishable.
		const uint64_t call = ++g_calls;
		if (call <= 20)
		{
			lodlight::Log("call %llu: map=%08x contentFlags=0x%X entities=%u rgbi.count=%u rgbi.data=%p positions.count=%u numStreetLights=%u category=%u",
				(unsigned long long)call, md->name, (unsigned)md->contentFlags, (unsigned)md->entities.count,
				(unsigned)dl.rgbi.count, (void*)dl.rgbi.data, (unsigned)dl.positions.count, (unsigned)dl.numStreetLights, (unsigned)dl.category);
		}

		const uint32_t n = dl.rgbi.count;
		if (n == 0 || dl.rgbi.data == nullptr)
			return;

		if (cfg.liveRepaint && idx >= 0)
		{
			std::vector<uint32_t> originals(dl.rgbi.data, dl.rgbi.data + n);
			lodlight::track::Register(lodlight::track::Ymap, store, static_cast<uint32_t>(idx), md, std::move(originals));
		}

		g_blocks++;
		uint32_t changed = 0;
		// Per block: the first few entries plus the first few *matches*, so a
		// block full of neon signage does not use up the whole sample budget
		// before a single street light is seen.
		unsigned sampledAny = 0, sampledMatch = 0;
		constexpr unsigned kPerBlock = 6;
		for (uint32_t i = 0; i < n; ++i)
		{
			uint32_t& entry = dl.rgbi.data[i];

			if (cfg.logSamples > 0 && g_samplesLogged < cfg.logSamples)
			{
				lodlight::RGB c = lodlight::Unpack(entry);
				lodlight::HSV h = lodlight::ToHSV(c);
				bool match = lodlight::Matches(c, cfg.match);
				bool want = (sampledAny < kPerBlock) || (match && sampledMatch < kPerBlock);
				if (want)
				{
					sampledAny++;
					if (match)
						sampledMatch++;
					g_samplesLogged++;
					if (dl.positions.data && i < dl.positions.count)
					{
						const Vec3& p = dl.positions.data[i];
						lodlight::Log("sample map=%08x [%u] raw=0x%08X rgb=(%.0f,%.0f,%.0f) top=%u hsv=(%.1f,%.2f,%.2f) pos=(%.1f,%.1f,%.1f) match=%d",
							md->name, i, entry, c.r, c.g, c.b, entry >> 24, h.h, h.s, h.v, p.x, p.y, p.z, match);
					}
					else
					{
						lodlight::Log("sample map=%08x [%u] raw=0x%08X rgb=(%.0f,%.0f,%.0f) top=%u hsv=(%.1f,%.2f,%.2f) match=%d",
							md->name, i, entry, c.r, c.g, c.b, entry >> 24, h.h, h.s, h.v, match);
					}
				}
			}

			if (lodlight::Recolor(entry, cfg.match))
				changed++;
		}

		g_lights += n;
		g_recolored += changed;

		if (cfg.logBlocks)
		{
			lodlight::Log("map=%08x lights=%u recolored=%u numStreetLights=%u positions=%u category=%u",
				md->name, n, changed, dl.numStreetLights, dl.positions.count, dl.category);
		}
	}

	void FinishLoadingHook(void* store, int32_t idx, CMapData** data)
	{
		const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));

		// Main thread, streaming is up: the right moment to hook the model
		// stores too (their FinishLoading is called from this same thread,
		// from the same template code that just called us).
		if (!g_nearInitTried)
		{
			g_nearInitTried = true;
			lodlight::Log("FinishLoading caller: %p (base+0x%llX) store=%p idx=%d", (void*)ret,
				(unsigned long long)(ret - g_imageBase), store, idx);

			// Full call chain (x64 unwind tables make this reliable), with each
			// frame related to the ymap store's vtable entries: the first frame
			// that lies inside a vtable function names the virtual the
			// streaming engine used to load this asset.
			{
				void* frames[24] = {};
				USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
				void** vt = *reinterpret_cast<void***>(store);
				for (USHORT i = 0; i < n; ++i)
				{
					uintptr_t f = reinterpret_cast<uintptr_t>(frames[i]);
					char where[96] = "";
					if (f >= g_imageBase && f < g_imageBase + g_imageSize)
					{
						int bestSlot = -1;
						uintptr_t bestDist = ~0ull;
						for (int k = 0; k < 12; ++k)
						{
							uintptr_t e = reinterpret_cast<uintptr_t>(vt[k]);
							if (e <= f && f - e < bestDist)
							{
								bestDist = f - e;
								bestSlot = k;
							}
						}
						if (bestSlot >= 0 && bestDist < 0x4000)
							snprintf(where, sizeof(where), "  ~ ymap vtable[%d] + 0x%llX", bestSlot, (unsigned long long)bestDist);
						// the 8 bytes before the return address: the call instruction
						char pre[40] = "";
						{
							const uint8_t* b = reinterpret_cast<const uint8_t*>(f - 8);
							size_t pp = 0;
							for (int j = 0; j < 8; ++j)
								pp += static_cast<size_t>(snprintf(pre + pp, sizeof(pre) - pp, "%02X ", b[j]));
						}
						lodlight::Log("  frame %2u: base+0x%llX%s  pre=[%s]", i, (unsigned long long)(f - g_imageBase), where, pre);
					}
					else
					{
						HMODULE mod = nullptr;
						char name[MAX_PATH] = "?";
						if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(f), &mod) && mod)
						{
							GetModuleFileNameA(mod, name, sizeof(name));
							const char* slash = strrchr(name, '\\');
							lodlight::Log("  frame %2u: %s+0x%llX", i, slash ? slash + 1 : name, (unsigned long long)(f - reinterpret_cast<uintptr_t>(mod)));
						}
						else
							lodlight::Log("  frame %2u: %p", i, frames[i]);
					}
				}
				char vtdump[1200] = "";
				size_t pos = 0;
				for (int k = 0; k < 32 && pos < sizeof(vtdump) - 80; ++k)
				{
					uintptr_t e = reinterpret_cast<uintptr_t>(vt[k]);
					if (e >= g_imageBase && e < g_imageBase + g_imageSize)
						pos += static_cast<size_t>(snprintf(vtdump + pos, sizeof(vtdump) - pos, " [%d]=+%llX", k, (unsigned long long)(e - g_imageBase)));
					else
					{
						HMODULE mod = nullptr;
						char name[MAX_PATH] = "?";
						if (e && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(e), &mod) && mod)
						{
							GetModuleFileNameA(mod, name, sizeof(name));
							const char* slash = strrchr(name, '\\');
							pos += static_cast<size_t>(snprintf(vtdump + pos, sizeof(vtdump) - pos, " [%d]=%s+%llX", k, slash ? slash + 1 : name, (unsigned long long)(e - reinterpret_cast<uintptr_t>(mod))));
						}
						else
							pos += static_cast<size_t>(snprintf(vtdump + pos, sizeof(vtdump) - pos, " [%d]=%llX", k, (unsigned long long)e));
					}
				}
				lodlight::Log("  ymap vtable:%s", vtdump);
			}
			if (lodlight::GetConfig().nearEnabled)
			{
				if (!lodlight::nearlights::Init(g_imageBase, g_imageSize, store, g_finishLoadingTarget, ret))
					lodlight::Log("near-light recolouring unavailable (see above); LOD lights unaffected");
			}
			else
			{
				lodlight::Log("near-light recolouring disabled at startup (near_enabled = 0); enable and restart to use it");
			}
		}

		if (data && *data)
			ProcessMapData(store, idx, *data);

		g_origFinishLoading(store, idx, data);
	}

	// Mirrors what MinHook's thread enumeration does, purely to log which
	// step FiveM interferes with. Nothing is suspended here.
	void LogThreadSnapshotDiagnostics()
	{
		SetLastError(0);
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE)
		{
			lodlight::Log("threads: CreateToolhelp32Snapshot failed, error=%lu (expected under FiveM; no thread suspension is used)", GetLastError());
			return;
		}
		THREADENTRY32 te{};
		te.dwSize = sizeof(te);
		unsigned ours = 0;
		if (Thread32First(snap, &te))
		{
			const DWORD pid = GetCurrentProcessId();
			do
			{
				if (te.th32OwnerProcessID == pid)
					ours++;
				te.dwSize = sizeof(te);
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
		lodlight::Log("threads: snapshot ok, %u threads in process", ours);
	}

	bool InstallMapHook()
	{
		uintptr_t base = 0;
		size_t size = 0;
		if (!lodlight::GetMainModuleRange(base, size))
		{
			lodlight::Log("could not read main module headers");
			return false;
		}
		lodlight::Log("main module base=0x%llX size=0x%llX", (unsigned long long)base, (unsigned long long)size);
		g_imageBase = base;
		g_imageSize = size;
		lodlight::track::Init(base, size);
		lodlight::track::SetRepaint(lodlight::track::Ymap, &RepaintMapData);

		lodlight::Pattern pat;
		if (!lodlight::ParsePattern(kFinishLoadingPattern, pat))
		{
			lodlight::Log("bad pattern text");
			return false;
		}

		auto hits = lodlight::FindPattern(pat, base, size, 2);
		if (hits.size() != 1)
		{
			lodlight::Log("pattern '%s' matched %u times (need exactly 1); not hooking. The game build may have changed, check Cfx LoadStreamingFile.cpp for the current pattern.",
				kFinishLoadingPattern, (unsigned)hits.size());
			return false;
		}

		void* target = reinterpret_cast<void*>(hits[0] + kFinishLoadingOffset);
		g_finishLoadingTarget = target;
		lodlight::Log("pattern at 0x%llX, fwMapDataStore::FinishLoading at 0x%llX (base+0x%llX)",
			(unsigned long long)hits[0], (unsigned long long)target, (unsigned long long)(reinterpret_cast<uintptr_t>(target) - base));

		LogThreadSnapshotDiagnostics();

		MH_STATUS st = MH_Initialize();
		if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
		{
			lodlight::Log("MH_Initialize failed: %s", MH_StatusToString(st));
			return false;
		}
		st = MH_CreateHook(target, reinterpret_cast<void*>(&FinishLoadingHook), reinterpret_cast<void**>(&g_origFinishLoading));
		if (st != MH_OK)
		{
			lodlight::Log("MH_CreateHook failed: %s", MH_StatusToString(st));
			return false;
		}
		st = MH_EnableHook(target);
		if (st != MH_OK)
		{
			lodlight::Log("MH_EnableHook failed: %s", MH_StatusToString(st));
			return false;
		}
		lodlight::Log("map hook installed (thread 0x%lX, threads frozen via: %s)", GetCurrentThreadId(), lodlight::LastFreezeMethod());
		return true;
	}

	// ------------------------------------------------------------------ worker

	DWORD WINAPI WorkerThread(LPVOID)
	{
		if (lodlight::GetConfig().menuKey > 0)
		{
			if (!lodlight::OverlayInit())
				lodlight::Log("menu unavailable; ini + reload key still work");
		}
		else
		{
			lodlight::Log("menu disabled (menu_key = 0)");
		}

		bool reloadWasDown = false;
		bool menuWasDown = false;
		for (;;)
		{
			Sleep(100);
			const lodlight::Config cfg = lodlight::GetConfig();

			if (cfg.reloadKey > 0)
			{
				bool down = (GetAsyncKeyState(cfg.reloadKey) & 0x8000) != 0;
				if (down && !reloadWasDown)
				{
					lodlight::Stats st = lodlight::GetStats();
					lodlight::Log("stats: calls=%llu blocks_with_lights=%llu lights=%llu recolored=%llu near_models=%llu near_lights=%llu near_recolored=%llu loaded_now=%llu",
						(unsigned long long)st.calls, (unsigned long long)st.blocksWithLights, (unsigned long long)st.lights,
						(unsigned long long)st.recolored, (unsigned long long)st.nearModels, (unsigned long long)st.nearLights,
						(unsigned long long)st.nearRecolored, (unsigned long long)st.loadedNow);
					if (lodlight::ReloadConfigFromDisk("hotkey"))
						lodlight::ReapplyAll();
				}
				reloadWasDown = down;
			}

			if (cfg.menuKey > 0)
			{
				bool down = (GetAsyncKeyState(cfg.menuKey) & 0x8000) != 0;
				if (down && !menuWasDown)
					lodlight::OverlayToggle();
				menuWasDown = down;
			}
		}
	}

	void Startup()
	{
		const std::wstring dir = PluginDir();
		g_configPath = dir + L"\\lodlight_recolor.ini";
		lodlight::LogInit(dir + L"\\lodlight_recolor.log");
		lodlight::Log("LodLightRecolor " LODLIGHT_VERSION " starting (thread 0x%lX)", GetCurrentThreadId());

		if (!lodlight::EnsureDefaultConfig(g_configPath))
			lodlight::Log("could not write default config to %ls", g_configPath.c_str());
		lodlight::ReloadConfigFromDisk("startup");

		if (!InstallMapHook())
		{
			lodlight::Log("inactive");
			return;
		}

		HANDLE h = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
		if (h)
			CloseHandle(h);
		else
			lodlight::Log("could not start worker thread; hotkeys and menu disabled");
	}
}

// ---------------------------------------------------------------------- Shared.h

namespace lodlight
{
	Config GetConfig()
	{
		AcquireSRWLockShared(&g_configLock);
		Config c = g_config;
		ReleaseSRWLockShared(&g_configLock);
		return c;
	}

	void SetConfig(const Config& in)
	{
		Config cfg = in;
		cfg.match.sourceHue = ToHSV(cfg.source).h;
		cfg.match.hueWindow = std::clamp(cfg.match.hueWindow, 0.f, 180.f);
		cfg.match.minSaturation = std::clamp(cfg.match.minSaturation, 0.f, 1.f);
		cfg.match.blend = std::clamp(cfg.match.blend, 0.f, 1.f);

		AcquireSRWLockExclusive(&g_configLock);
		g_config = cfg;
		ReleaseSRWLockExclusive(&g_configLock);
	}

	bool ReloadConfigFromDisk(const char* why)
	{
		Config cfg;
		std::string report;
		if (!LoadConfig(g_configPath, cfg, report))
		{
			Log("config: could not open %ls (%s), keeping current values", g_configPath.c_str(), why);
			return false;
		}
		SetConfig(cfg);
		g_samplesLogged = 0;
		LogConfig(GetConfig(), why);
		if (!report.empty())
			Log("config warnings:\n%s", report.c_str());
		return true;
	}

	bool SaveConfigToDisk(std::string& error)
	{
		bool ok = SaveConfig(g_configPath, GetConfig(), error);
		Log(ok ? "config saved to %ls" : "config save to %ls failed", g_configPath.c_str());
		return ok;
	}

	const std::wstring& ConfigPath()
	{
		return g_configPath;
	}

	void ReapplyAll()
	{
		const Config cfg = GetConfig();
		track::Totals t = track::ReapplyAll(cfg);
		g_lastRepaintLights = t.lights;
		g_lastRepaintRecolored = t.changed;
		if (cfg.logBlocks || t.stale || t.mismatched)
			Log("repainted %llu loaded objects: %llu lights, %llu recolored; dropped %llu stale, %llu mismatched",
				(unsigned long long)t.objects, (unsigned long long)t.lights, (unsigned long long)t.changed,
				(unsigned long long)t.stale, (unsigned long long)t.mismatched);
	}

	Stats GetStats()
	{
		Stats s;
		s.calls = g_calls.load();
		s.blocksWithLights = g_blocks.load();
		s.lights = g_lights.load();
		s.recolored = g_recolored.load();
		s.lastRepaintLights = g_lastRepaintLights.load();
		s.lastRepaintRecolored = g_lastRepaintRecolored.load();
		s.nearModels = nearlights::Models();
		s.nearLights = nearlights::Lights();
		s.nearRecolored = nearlights::Recolored();
		s.loadedNow = track::CountLoaded();
		return s;
	}

	bool NearAvailable()
	{
		return nearlights::Available();
	}

	const char* LastFreezeMethod()
	{
		switch (mh_lodlight_freeze_method)
		{
		case 1: return "Toolhelp";
		case 2: return "NtGetNextThread";
		case 0: return "none (not suspended)";
		default: return "unknown";
		}
	}
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_module = instance;
		DisableThreadLibraryCalls(instance);
		Startup();
	}
	return TRUE;
}
