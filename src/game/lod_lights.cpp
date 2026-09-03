// lod_lights.cpp - see lod_lights.h.
//
// Cfx has already detoured FinishLoading when we arrive; MinHook detours the
// detour, so this hook runs first and Cfx's numStreetLights fix-up runs
// after. We never rely on that fix-up.
#include "game/lod_lights.h"
#include "color/recolor.h"
#include "game/near_lights.h"
#include "game/structs.h"
#include "game/track.h"
#include "hook/pattern.h"
#include "plugin/log.h"
#include "plugin/plugin.h"

#include <windows.h>
#include <tlhelp32.h>
#include <MinHook.h>

#include <atomic>
#include <vector>

namespace lodlight::lodlights
{
	namespace
	{
		FinishLoadingFn g_orig = nullptr;
		uintptr_t g_imageBase = 0;
		size_t g_imageSize = 0;
		bool g_nearInitTried = false;

		std::atomic<int> g_samplesLogged{ 0 };
		std::atomic<uint64_t> g_calls{ 0 };
		std::atomic<uint64_t> g_blocks{ 0 };
		std::atomic<uint64_t> g_lights{ 0 };
		std::atomic<uint64_t> g_recolored{ 0 };

		// track::RepaintFn for ymap blocks.
		bool Repaint(void* obj, const std::vector<uint32_t>& originals, const Config& cfg, uint64_t& lights, uint64_t& changed)
		{
			CMapData* md = static_cast<CMapData*>(obj);
			CDistantLODLight& dl = md->distantLodLights;
			if (dl.rgbi.data == nullptr || dl.rgbi.count != originals.size())
				return false;
			for (size_t i = 0; i < originals.size(); ++i)
			{
				uint32_t v = originals[i];
				if (Recolor(v, cfg.match))
					changed++;
				dl.rgbi.data[i] = v;
			}
			lights += originals.size();
			return true;
		}

		void Process(void* store, int32_t idx, CMapData* md)
		{
			const Config cfg = GetConfig();
			CDistantLODLight& dl = md->distantLodLights;

			// The first calls are logged regardless of content so "hook never
			// fires" and "every block was empty" are distinguishable.
			const uint64_t call = ++g_calls;
			if (call <= 20)
			{
				Log("call %llu: map=%08x contentFlags=0x%X entities=%u rgbi.count=%u rgbi.data=%p positions.count=%u numStreetLights=%u category=%u",
					(unsigned long long)call, md->name, (unsigned)md->contentFlags, (unsigned)md->entities.count,
					(unsigned)dl.rgbi.count, (void*)dl.rgbi.data, (unsigned)dl.positions.count, (unsigned)dl.numStreetLights, (unsigned)dl.category);
			}

			const uint32_t n = dl.rgbi.count;
			if (n == 0 || dl.rgbi.data == nullptr)
				return;

			if (cfg.liveRepaint && idx >= 0)
			{
				std::vector<uint32_t> originals(dl.rgbi.data, dl.rgbi.data + n);
				track::Register(track::Ymap, store, static_cast<uint32_t>(idx), md, std::move(originals));
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
					RGB c = Unpack(entry);
					HSV h = ToHSV(c);
					bool match = Matches(c, cfg.match);
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
							Log("sample map=%08x [%u] raw=0x%08X rgb=(%.0f,%.0f,%.0f) top=%u hsv=(%.1f,%.2f,%.2f) pos=(%.1f,%.1f,%.1f) match=%d",
								md->name, i, entry, c.r, c.g, c.b, entry >> 24, h.h, h.s, h.v, p.x, p.y, p.z, match);
						}
						else
						{
							Log("sample map=%08x [%u] raw=0x%08X rgb=(%.0f,%.0f,%.0f) top=%u hsv=(%.1f,%.2f,%.2f) match=%d",
								md->name, i, entry, c.r, c.g, c.b, entry >> 24, h.h, h.s, h.v, match);
						}
					}
				}

				if (Recolor(entry, cfg.match))
					changed++;
			}

			g_lights += n;
			g_recolored += changed;

			if (cfg.logBlocks)
			{
				Log("map=%08x lights=%u recolored=%u numStreetLights=%u positions=%u category=%u",
					md->name, n, changed, dl.numStreetLights, dl.positions.count, dl.category);
			}
		}

		void Hook(void* store, int32_t idx, CMapData** data)
		{
			// Main thread, streaming is up: the right moment to hook the model
			// stores too (their load-complete virtual is called from this thread).
			if (!g_nearInitTried)
			{
				g_nearInitTried = true;
				if (GetConfig().nearEnabled)
				{
					if (!nearlights::Init(g_imageBase, g_imageSize, store))
						Log("near-light recolouring unavailable (see above); LOD lights unaffected");
				}
				else
				{
					Log("near-light recolouring disabled at startup (near_enabled = 0); enable and restart to use it");
				}
			}

			if (data && *data)
				Process(store, idx, *data);

			g_orig(store, idx, data);
		}

		// Purely to record what FiveM does to the Toolhelp snapshot. Nothing is
		// suspended here.
		void LogThreadSnapshotDiagnostics()
		{
			SetLastError(0);
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap == INVALID_HANDLE_VALUE)
			{
				Log("threads: CreateToolhelp32Snapshot failed, error=%lu (expected under FiveM; no thread suspension is used)", GetLastError());
				return;
			}
			CloseHandle(snap);
			Log("threads: snapshot ok");
		}
	}

	bool Install()
	{
		if (!GetMainModuleRange(g_imageBase, g_imageSize))
		{
			Log("could not read main module headers");
			return false;
		}
		Log("main module base=0x%llX size=0x%llX", (unsigned long long)g_imageBase, (unsigned long long)g_imageSize);
		track::Init(g_imageBase, g_imageSize);
		track::SetRepaint(track::Ymap, &Repaint);

		Pattern pat;
		if (!ParsePattern(kFinishLoadingPattern, pat))
		{
			Log("bad pattern text");
			return false;
		}
		auto hits = FindPattern(pat, g_imageBase, g_imageSize, 2);
		if (hits.size() != 1)
		{
			Log("pattern '%s' matched %u times (need exactly 1); not hooking. The game build may have changed, check Cfx LoadStreamingFile.cpp for the current pattern.",
				kFinishLoadingPattern, (unsigned)hits.size());
			return false;
		}

		void* target = reinterpret_cast<void*>(hits[0] + kFinishLoadingOffset);
		Log("fwMapDataStore::FinishLoading at 0x%llX (base+0x%llX)",
			(unsigned long long)target, (unsigned long long)(reinterpret_cast<uintptr_t>(target) - g_imageBase));

		LogThreadSnapshotDiagnostics();

		MH_STATUS st = MH_Initialize();
		if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
		{
			Log("MH_Initialize failed: %s", MH_StatusToString(st));
			return false;
		}
		st = MH_CreateHook(target, reinterpret_cast<void*>(&Hook), reinterpret_cast<void**>(&g_orig));
		if (st == MH_OK)
			st = MH_EnableHook(target);
		if (st != MH_OK)
		{
			Log("hooking FinishLoading failed: %s", MH_StatusToString(st));
			return false;
		}
		Log("map hook installed (thread 0x%lX, threads frozen via: %s)", GetCurrentThreadId(), LastFreezeMethod());
		return true;
	}

	LodStats GetStats()
	{
		LodStats s;
		s.calls = g_calls.load();
		s.blocksWithLights = g_blocks.load();
		s.lights = g_lights.load();
		s.recolored = g_recolored.load();
		return s;
	}

	void ResetSampleBudget()
	{
		g_samplesLogged = 0;
	}
}
