// GameStructs.h - the two RAGE structs the hook touches.
//
// Layouts are taken from citizenfx/fivem
//   code/components/gta-streaming-five/include/EntitySystem.h
// (CDistantLODLight and CMapData, with Cfx's own "+N" offset comments) and
// confirmed in-game. The static_asserts below make a layout mistake a
// compile error instead of a silent bad write.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rage
{
	// Standard RAGE array header: pointer, used count, capacity.
	template <typename T>
	struct atArray
	{
		T* data;
		uint16_t count;
		uint16_t capacity;
	};
	static_assert(sizeof(atArray<int>) == 16, "atArray");
}

struct Vec3
{
	float x, y, z;
};

struct CDistantLODLight
{
	void* vtable;
	rage::atArray<Vec3> positions;     // +8
	rage::atArray<uint32_t> rgbi;      // +24  (0xIIRRGGBB, see Recolor.h)
	uint16_t numStreetLights;          // +40
	uint16_t category;                 // +42
	uint32_t pad_2C;
};
static_assert(offsetof(CDistantLODLight, positions) == 8, "CDistantLODLight::positions");
static_assert(offsetof(CDistantLODLight, rgbi) == 24, "CDistantLODLight::rgbi");
static_assert(offsetof(CDistantLODLight, numStreetLights) == 40, "CDistantLODLight::numStreetLights");
static_assert(offsetof(CDistantLODLight, category) == 42, "CDistantLODLight::category");
static_assert(sizeof(CDistantLODLight) == 48, "CDistantLODLight size");

struct Vec4
{
	float v[4];
};

struct CMapData
{
	void* vtable;
	uint32_t name;         // +8   joaat hash of the ymap name
	uint32_t parent;       // +12
	int32_t flags;         // +16
	int32_t contentFlags;  // +20  bit 7 = LOD lights, bit 8 = distant LOD lights
	uint8_t pad_18[8];
	Vec4 streamingExtentsMin; // +32
	Vec4 streamingExtentsMax; // +48
	Vec4 entitiesExtentsMin;  // +64
	Vec4 entitiesExtentsMax;  // +80
	rage::atArray<void*> entities; // +96 (fwEntityDef*)
	uint8_t pad_70[392 - 112];
	CDistantLODLight distantLodLights; // +392
};
static_assert(offsetof(CMapData, name) == 8, "CMapData::name");
static_assert(offsetof(CMapData, entities) == 96, "CMapData::entities");
static_assert(offsetof(CMapData, distantLodLights) == 392, "CMapData::distantLodLights");

// fwMapDataStore::FinishLoading(strStreamingModule* store, int32_t idx, CMapData** data)
using FinishLoadingFn = void(*)(void* store, int32_t idx, CMapData** data);

// Cfx's own locator for that function, verbatim from
// code/components/gta-streaming-five/src/LoadStreamingFile.cpp:
//   MH_CreateHook(hook::get_pattern("25 00 0C 00 00 3D 00 08 00 00 49 8B 06", -0x6F), ...)
// get_pattern() returns (unique match) + offset, so the function starts 0x6F
// bytes before the match.
constexpr const char* kFinishLoadingPattern = "25 00 0C 00 00 3D 00 08 00 00 49 8B 06";
constexpr ptrdiff_t kFinishLoadingOffset = -0x6F;
