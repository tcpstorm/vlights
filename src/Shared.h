// Shared.h - the plugin core's interface to the overlay (and vice versa).
#pragma once

#include "Config.h"

#include <cstdint>
#include <string>

namespace lodlight
{
	struct Stats
	{
		uint64_t calls = 0;            // FinishLoading hook invocations
		uint64_t blocksWithLights = 0; // cumulative blocks that had a light array
		uint64_t lights = 0;           // cumulative lights walked at load time
		uint64_t recolored = 0;        // cumulative recolored at load time
		uint64_t loadedNow = 0;        // blocks currently tracked (loaded)
		uint64_t nearModels = 0;       // models with lights seen at placement
		uint64_t nearLights = 0;
		uint64_t nearRecolored = 0;
		uint64_t lastRepaintLights = 0;
		uint64_t lastRepaintRecolored = 0;
	};

	// --- implemented in Main.cpp ---
	Config GetConfig();                        // snapshot
	void SetConfig(const Config& cfg);         // replace; recomputes derived fields
	void ReapplyAll();                         // restore originals + recolor every loaded block
	Stats GetStats();
	bool ReloadConfigFromDisk(const char* why);
	bool SaveConfigToDisk(std::string& error);
	const std::wstring& ConfigPath();
	const char* LastFreezeMethod();            // how MinHook froze threads on the last enable
	bool NearAvailable();                      // model-light hooks installed this session

	// --- implemented in Overlay.cpp ---
	// Starts the menu's own window + UI thread (no game hooks). Call once
	// from a worker thread after the map hook is installed, never from DllMain.
	bool OverlayInit();
	void OverlayToggle();
	bool OverlayVisible();
}
