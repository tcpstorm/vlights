// near_lights.h - recolours the light attributes baked into models (the
// lights on the actual lamp-post props), so the near tier matches the LOD
// tier. See near_lights.cpp for how.
#pragma once

#include <cstddef>
#include "plugin/config.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vlights::nearlights
{
	// Finds the ydr/yft/ydd streaming stores and detours their load-complete
	// virtual. `knownYmapStore` is the strStreamingModule the map hook
	// receives; every discovery step is validated against it. Main thread
	// only (the first map load hook call is where it is done). Returns false
	// if nothing could be hooked.
	bool Init(uintptr_t imageBase, size_t imageSize, void* knownYmapStore);

	// Archetype (.ytyp) light-effect extensions. Archetype files load before
	// the first map block, so their hook must go in from DllMain: the ymap
	// FinishLoading's call site is located by scanning for calls to it, and
	// the sibling call (the archetype store's own FinishLoading) is detoured
	// from the same template code. Main thread, before the world loads.
	bool InstallTypesHook(uintptr_t imageBase, size_t imageSize, void* knownFinishLoading);

	bool Available();

	// Warm (hue 10..60) light colours that matched nothing this session, with
	// counts: what is still orange, by RGB.
	std::string UnmatchedWarmSummary();

	// Debug: type hashes of an entity's/archetype's extensions, with light
	// colours for light-effect ones.
	std::string DescribeExtensions(void* const* exts, unsigned n);

	// Light-effect extensions on a map block's entities (rage::fwEntityDef,
	// the per-placement light overrides that vanilla ymaps carry for many
	// lamp posts). Recolours them in place; appends (colour, volume colour)
	// pairs to `originals`. Returns the number of light definitions found.
	// Call inside the ymap FinishLoading hook only.
	uint32_t RecolourEntityLights(uint32_t mapName, void* const* entities, uint32_t count, const Config& cfg, std::vector<uint32_t>& originals);
	// Restore from originals[offset..] and recolour again. False if the block
	// no longer matches what was recorded.
	bool RepaintEntityLights(void* const* entities, uint32_t count, const std::vector<uint32_t>& originals, size_t offset, const Config& cfg, uint64_t& lights, uint64_t& changed);
	uint64_t EntityBlocks();

	uint64_t Models();
	uint64_t Lights();
	uint64_t Recolored();
}
