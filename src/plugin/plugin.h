// plugin.h - the plugin core's interface: config state, repaint, stats, and
// the menu entry points. Implemented in plugin.cpp (core) and ui/menu.cpp
// (menu).
#pragma once

#include "plugin/config.h"
#include <vlights/version.h>

#include <cstdint>
#include <memory>
#include <string>


namespace vlights
{
	struct Stats
	{
		uint64_t calls = 0;            // map load hook invocations
		uint64_t blocksWithLights = 0; // cumulative map blocks that had a light array
		uint64_t lights = 0;           // cumulative LOD lights walked at load time
		uint64_t recolored = 0;        // cumulative LOD lights recolored at load time
		uint64_t nearModels = 0;       // models with lights seen at load
		uint64_t nearLights = 0;
		uint64_t nearRecolored = 0;
		uint64_t loadedNow = 0;        // objects currently tracked for live repaint
		uint64_t lastRepaintLights = 0;
		uint64_t lastRepaintRecolored = 0;
	};

	// --- plugin.cpp ---
	void SetConfigPath(const std::wstring& path);
	const std::wstring& ConfigPath();
	Config GetConfig();                        // snapshot by value (menu, startup)
	// Shared snapshot for the hook paths: no string copies per map block,
	// model or texture placement. Replaced wholesale by SetConfig.
	using ConfigPtr = std::shared_ptr<const Config>;
	ConfigPtr GetConfigPtr();
	void SetConfig(const Config& cfg);         // replace; recomputes derived fields
	bool ReloadConfigFromDisk(const char* why);
	bool SaveConfigToDisk(std::string& error);
	void ReapplyAll();                         // restore originals + recolour everything loaded
	Stats GetStats();
	const char* LastFreezeMethod();            // how MinHook froze threads on the last enable
	bool NearAvailable();                      // model-light hooks installed this session

	// --- ui/menu.cpp ---
	// Starts the menu's own window + UI thread (no game hooks). Call once from
	// a worker thread after the map hook is installed, never from DllMain.
	bool MenuInit();
	void MenuToggle();
	bool MenuVisible();
}
