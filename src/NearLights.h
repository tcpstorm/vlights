// NearLights.h - recolours the light attributes baked into models (the
// lights on the actual lamp-post props), so the near tier matches the LOD
// tier. See NearLights.cpp for how.
#pragma once

#include <cstddef>
#include <cstdint>

namespace lodlight::nearlights
{
	// Finds the ydr/yft/ydd streaming stores through Cfx's public patterns
	// and detours their PlaceResource. Main thread only (the first map load
	// hook call is a good place). Returns false if nothing could be hooked.
	// `knownYmapStore` is the strStreamingModule the map hook already sees;
	// a candidate manager/lookup pair is accepted only if asking it for
	// "ymap" returns exactly that pointer.
	// `knownFinishLoading` is fwMapDataStore::FinishLoading (from Cfx's
	// pattern); its vtable slot on the ymap store is the slot hooked on the
	// model stores.
	// `callSiteReturn` is the return address seen inside the ymap FinishLoading
	// hook: the call instruction just before it is the template for finding
	// every other store type's FinishLoading call.
	bool Init(uintptr_t imageBase, size_t imageSize, void* knownYmapStore, void* knownFinishLoading, uintptr_t callSiteReturn);
	bool Available();

	uint64_t Models();
	uint64_t Lights();
	uint64_t Recolored();
}
