// lod_lights.h - the distant / LOD light recolour: a detour on
// fwMapDataStore::FinishLoading that rewrites CDistantLODLight::rgbi as each
// map block is parsed. This is the plugin's first and primary hook; the
// model-light hooks are installed from inside it.
#pragma once

#include "plugin/config.h"

#include <cstdint>

namespace lodlight::lodlights
{
	struct LodStats
	{
		uint64_t calls = 0;
		uint64_t blocksWithLights = 0;
		uint64_t lights = 0;
		uint64_t recolored = 0;
	};

	// Locates the function through Cfx's public byte pattern and detours it.
	// Call from DllMain (main thread; the target's only caller is blocked in
	// LoadLibrary at that point). Returns false if the plugin should stay inactive.
	bool Install();

	LodStats GetStats();
	void ResetSampleBudget(); // log_samples counts from zero again
}
