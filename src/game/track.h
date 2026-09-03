// Track.h - registry of loaded streaming objects we have recoloured, so a
// config change can restore their original colours and recolour them again
// in place ("live repaint").
//
// One registry per streaming store kind (ymap, ydr, yft, ydd). Each store's
// strStreamingModule::Remove (vtable slot 3 on GTA V per Cfx's Streaming.h)
// is detoured *by code*, with the address read from the store's vtable, so
// an unloading slot is dropped before the game frees the object. No vtable
// is ever written to: FiveM's anti-cheat terminated the game when an
// earlier build did that.
//
// Before any repaint write, the slot is also checked read-only against the
// store's asset pool (Cfx: atPoolBase at store+56; entry name hash at +12,
// object pointer at +0 where the store keeps it there).
#pragma once

#include "plugin/config.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lodlight::track
{
	enum Kind : int
	{
		Ymap = 0,
		Ydr = 1,
		Yft = 2,
		Ydd = 3,
		Ytyp = 4,
		KindCount = 5
	};

	// Restore `originals` into `obj` and recolour per `cfg`. Returns false if
	// `obj` no longer matches `originals` (nothing must be written then).
	using RepaintFn = bool (*)(void* obj, const std::vector<uint32_t>& originals, const Config& cfg, uint64_t& lights, uint64_t& changed);

	struct Totals
	{
		uint64_t objects = 0, lights = 0, changed = 0, stale = 0, mismatched = 0;
	};

	void Init(uintptr_t imageBase, size_t imageSize);
	void SetRepaint(Kind k, RepaintFn fn);

	// Remember `obj`, currently loaded in slot `idx` of `store`. Installs the
	// store's Remove detour on first use. Call from the main thread only.
	void Register(Kind k, void* store, uint32_t idx, void* obj, std::vector<uint32_t>&& originals);

	// Restore + recolour everything still loaded. Any thread.
	Totals ReapplyAll(const Config& cfg);

	uint64_t CountLoaded();

	// Visit every object of kind `k` still loaded (liveness checked against
	// the pool first). Any thread; the registry lock is held during the walk.
	using VisitFn = void (*)(void* obj, uint32_t idx, void* user);
	void ForEachLoaded(Kind k, VisitFn fn, void* user);
	const char* KindName(Kind k);
}
