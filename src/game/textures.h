// textures.h - the lantern glow. A lamp's lens is lit by an emissive texture,
// not by a light, so the light colour never reaches it. The texture is
// either in a texture dictionary (.ytd) or, for most lamp props, embedded in
// the model file itself (prop_streetlight_01 embeds prop_streetlight_head).
//
// Both are reached the same way: a detour on the streaming store's
// PlaceResource (Cfx Streaming.h: base slot 6, +6 on builds >= 2802), which
// receives the raw resource block map before the game fixes up pointers and
// creates the GPU textures. The raw data is walked with the file's own
// encoded pointers, and the selected textures' pixels are hue-shifted in
// place, DXT endpoints included, across every mip level.
//
// Live repaint: the GPU texture made from that data is immutable, so a
// colour change cannot update it. Instead every selected texture is
// registered at placement with a copy of its original pixels and the
// address its texture object will have (placement is in place, so the
// block map already says). On a change, the original is recoloured again
// with the new settings, a fresh GPU texture + view are created on the
// game's device (device calls are free-threaded; no context, no render
// hook), and the two pointers in the texture object are swapped. Every GPU object we made for
// a texture stays alive until that texture unloads: the game's own
// hi-detail swap moves resource pointers between texture objects and
// restores them later, so a released pointer could resurface. Unloads
// arrive through the same Remove detour as the light registry (and, as a
// backstop, the store's pool is checked before each repaint) and drop the
// entry after putting the game's own pointers back.
//
// Textures are selected by name substring (`texture_names`, default
// "streetlight", "wall_light", ...) so amber vehicle indicators and signage
// stay as they are.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vlights::textures
{
	enum StoreKind : int
	{
		Txd = 0, // pgDictionary<grcTexture>
		Ydr = 1, // gtaDrawable (embedded dictionary via its shader group)
		Yft = 2, // gtaFragType (its drawable, plus the extra drawables array)
		Ydd = 3, // pgDictionary<gtaDrawable>
		StoreKindCount = 4
	};

	// Main thread, inside the map hook (same rules as the model-store hooks).
	// FiveM already owns some of these slots with runtime-generated handlers;
	// those are followed to the game routine they call, which is detoured.
	bool InstallPlacementHook(StoreKind kind, void* store, int placeSlot, uintptr_t imageBase, size_t imageSize);

	// track::RemoveListener: a slot is being unloaded (main thread, before
	// the game frees it).
	void OnStoreRemove(void* store, uint32_t idx);

	// Ask for every registered texture to be rebuilt with the current
	// config. Coalesced: the work runs from Tick() at most a few times a
	// second, so a slider drag does not create textures every frame.
	void RequestRepaint();

	// Worker thread, every ~100 ms: performs pending repaints.
	void Tick();

	uint64_t Placements();         // PlaceResource calls seen on hooked stores
	uint64_t Dictionaries();       // of those, resources holding a selected texture
	uint64_t TexturesRecoloured(); // selected textures with at least one pixel/block changed
	uint64_t BlocksRecoloured();   // DXT blocks or pixels changed in total
	uint64_t Registered();         // textures currently registered for live repaint
}
