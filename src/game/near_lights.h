// near_lights.h - recolours the light attributes baked into models (the
// lights on the actual lamp-post props), so the near tier matches the LOD
// tier. See near_lights.cpp for how.
#pragma once

#include <cstddef>
#include <cstdint>

namespace lodlight::nearlights
{
	// Finds the ydr/yft/ydd streaming stores and detours their load-complete
	// virtual. `knownYmapStore` is the strStreamingModule the map hook
	// receives; every discovery step is validated against it. Main thread
	// only (the first map load hook call is where it is done). Returns false
	// if nothing could be hooked.
	bool Init(uintptr_t imageBase, size_t imageSize, void* knownYmapStore);
	bool Available();

	uint64_t Models();
	uint64_t Lights();
	uint64_t Recolored();
}
