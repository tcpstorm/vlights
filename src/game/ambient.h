// ambient.h - the baked road glow. Roads, pavements and facades around
// lamp posts carry a vertex-painted "artificial ambient" weight, and at
// night the deferred ambient pass lights that weight with the timecycle's
// light_artificial_ext_{down,up}_col colour (sodium orange in every vanilla
// weather). It is not a light and not a texture: once every lamp, LOD
// light and lens is white, the ground under them is still orange at a
// distance. The only way to change it is to change the timecycle value.
//
// Done the way the game's own scripts do it: a timecycle modifier
// (rage::tcModifier) holding the six colour variables is appended to the
// timecycle manager's modifier list and put in the primary script slot
// (what SET_TIMECYCLE_MODIFIER writes; the extra slot is cleared by the
// game every frame). The modifier is applied by the game
// every frame on top of the weather keyframes, so the colour follows the
// target instantly. No detour, no vtable write: data only, written from the
// main thread inside the map hook (the list) and plain aligned stores
// afterwards (the colour values, the slot index).
//
// Everything the game may hold a pointer to is allocated with the game's
// own allocator (rage::sysMemAllocator from the executable's TLS slot, as
// Cfx's sysAllocator.cpp does), so the game may grow or drop the list
// later without freeing our heap.
//
// Layouts and byte patterns are Cfx's (gta-game-five/include/Timecycle.h,
// src/Timecycle.cpp, rage-allocator-five/src/sysAllocator.cpp).
#pragma once

#include "plugin/config.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace vlights::ambient
{
	// DllMain (after the map hook): locates the timecycle statics by byte
	// pattern. Reads only; nothing is touched until the first map block.
	void Install(uintptr_t imageBase, size_t imageSize);

	// Main thread, inside the map hook, every call: validates the manager
	// on the first call, registers the modifier when the feature is on,
	// re-registers it if the manager dropped it. Returns quickly once done
	// or once permanently unavailable.
	void OnMapHook();

	// Any thread: write the colour for `cfg` into the modifier and claim or
	// release the primary slot according to `cfg`.
	void Apply(const Config& cfg);

	// Worker thread, every ~100 ms: keep the slot ours if a script cleared
	// it, follow the modifier if the list was reordered.
	void Tick();

	// One line for the F9 block and the menu.
	std::string Status();
	bool Available();
}
