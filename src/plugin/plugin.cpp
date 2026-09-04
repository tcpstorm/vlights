// plugin.cpp - config state shared by the hooks, the menu and the hotkeys;
// live repaint; statistics.
#include "plugin/plugin.h"
#include "color/recolor.h"
#include "game/lod_lights.h"
#include "game/near_lights.h"
#include "game/textures.h"
#include "game/track.h"
#include "plugin/log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>

extern "C" int mh_vlights_freeze_method; // from the patched MinHook

namespace
{
	SRWLOCK g_configLock = SRWLOCK_INIT;
	vlights::Config g_config;
	std::wstring g_configPath;

	std::atomic<uint64_t> g_lastRepaintLights{ 0 };
	std::atomic<uint64_t> g_lastRepaintRecolored{ 0 };

	void LogConfig(const vlights::Config& cfg, const char* why)
	{
		vlights::Log("config (%s): enabled=%d source=(%.0f,%.0f,%.0f) hue=%.1f window=%.1f min_sat=%.2f target=(%.0f,%.0f,%.0f) blend=%.2f keep_brightness=%d cream=%d near=%d near_log=%d log_samples=%d log_blocks=%d reload_key=0x%X menu_key=0x%X live_repaint=%d debug=%d",
			why, cfg.match.enabled, cfg.source.r, cfg.source.g, cfg.source.b, cfg.match.sourceHue, cfg.match.hueWindow,
			cfg.match.minSaturation, cfg.match.target.r, cfg.match.target.g, cfg.match.target.b, cfg.match.blend,
			cfg.match.keepBrightness, cfg.match.zone2, cfg.nearEnabled, cfg.nearLog, cfg.logSamples, cfg.logBlocks, cfg.reloadKey, cfg.menuKey, cfg.liveRepaint, cfg.debug);
	}
}

namespace vlights
{
	void SetConfigPath(const std::wstring& path)
	{
		g_configPath = path;
	}

	const std::wstring& ConfigPath()
	{
		return g_configPath;
	}

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
		cfg.match.source2Hue = ToHSV(cfg.source2).h;
		cfg.match.hueWindow = std::clamp(cfg.match.hueWindow, 0.f, 180.f);
		cfg.match.minSaturation = std::clamp(cfg.match.minSaturation, 0.f, 1.f);
		cfg.match.blend = std::clamp(cfg.match.blend, 0.f, 1.f);

		SetDebugLogging(cfg.debug);

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
		lodlights::ResetSampleBudget();
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

	void ReapplyAll()
	{
		const Config cfg = GetConfig();
		track::Totals t = track::ReapplyAll(cfg);
		textures::RequestRepaint();
		g_lastRepaintLights = t.lights;
		g_lastRepaintRecolored = t.changed;
		if (cfg.logBlocks || t.stale || t.mismatched)
			LogDebug("repainted %llu loaded objects: %llu lights, %llu recolored; dropped %llu stale, %llu mismatched",
				(unsigned long long)t.objects, (unsigned long long)t.lights, (unsigned long long)t.changed,
				(unsigned long long)t.stale, (unsigned long long)t.mismatched);
	}

	Stats GetStats()
	{
		Stats s;
		lodlights::LodStats l = lodlights::GetStats();
		s.calls = l.calls;
		s.blocksWithLights = l.blocksWithLights;
		s.lights = l.lights;
		s.recolored = l.recolored;
		s.nearModels = nearlights::Models();
		s.nearLights = nearlights::Lights();
		s.nearRecolored = nearlights::Recolored();
		s.loadedNow = track::CountLoaded();
		s.lastRepaintLights = g_lastRepaintLights.load();
		s.lastRepaintRecolored = g_lastRepaintRecolored.load();
		return s;
	}

	bool NearAvailable()
	{
		return nearlights::Available();
	}

	const char* LastFreezeMethod()
	{
		switch (mh_vlights_freeze_method)
		{
		case 1: return "Toolhelp";
		case 2: return "NtGetNextThread";
		case 0: return "none (not suspended)";
		default: return "unknown";
		}
	}
}
