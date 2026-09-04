# LOD lights (distant and mid-range tiers)

The lamps you see across the city at night, beyond the range where the
actual lamp entities exist. Two tiers share one data source.

## Where the colour lives

Every `.ymap` block is parsed by the game into a `CMapData`. Blocks named
`*_lodlights.ymap` and `*_distantlights.ymap` carry a struct-of-arrays of
distant LOD lights:

```
CMapData (rage::fwMapData)
  +8    uint32 name          joaat of the block name
  +12   uint32 parent
  +16   int32  flags
  +20   int32  contentFlags  bit 7 = LOD lights, bit 8 = distant LOD lights
  +32   Vec4   streamingExtentsMin / +48 Max
  +64   Vec4   entitiesExtentsMin  / +80 Max
  +96   atArray<fwEntityDef*> entities   (ptr, u16 count at +104)
  +392  CDistantLODLight distantLodLights

CDistantLODLight (48 bytes)
  +0    vtable
  +8    atArray<Vec3>   positions   (ptr, u16 count at +16)
  +24   atArray<uint32> rgbi        (ptr, u16 count at +32)
  +40   uint16 numStreetLights      first N entries are street lights
  +42   uint16 category             0 = lodlights block, 1 = distantlights
```

`rgbi` entries are `0xIIRRGGBB`: intensity in the top byte, then red, green,
blue. Vanilla sodium decodes as `0xAAFF780A` = intensity 170, RGB
(255,120,10). Confirmed in-game with a green test target.

The mid-range tier (`CLODLight` in `LODLightsSOA`: direction, falloff,
cone angles, flags) has no colour of its own; it takes position and colour
from the `DistantLODLightsSOA` entry with the same index in the same block.
So recolouring `rgbi` changes both tiers.

## The hook

`fwMapDataStore::FinishLoading(store, int32 idx, CMapData** data)` is
called on the main thread for every block right after parsing. Cfx locates
it with the byte pattern in its own `LoadStreamingFile.cpp`
(`25 00 0C 00 00 3D 00 08 00 00 49 8B 06`, function starts 0x6F bytes
before the match) and detours it to fix a `numStreetLights` bug. This
plugin detours the same address with MinHook from `DllMain`, so our hook
runs first, then Cfx's, then the game's.

`src/game/lod_lights.cpp`:

- `Process()` walks `rgbi[]` and rewrites every entry the matcher accepts
  (`src/color/recolor.h`, HSV two-zone matcher) before the game reads it.
- The same call is where the entity light overrides are handled
  (near-lights.md) and where the near-light hooks get installed on the
  first call (the streaming manager is up by then).
- With `live_repaint = 1` the block is registered in `src/game/track.cpp`
  with its original colours, keyed by store + slot index.

## Live repaint

F9 or a menu change calls `track::ReapplyAll`: for each registered block
that is still loaded, restore the originals and recolour again with the
new settings. The arrays are read by the game in place, so both LOD tiers
change on the next frame.

Liveness is two checks: the store's asset pool entry (`atPoolBase` at
store+56; entry name hash at +12) must still carry the registered name
hash, and the block must not have been unloaded. Unloads are heard through
a detour on the store's `strStreamingModule::Remove` (slot 9 on b3751; see
quirks.md for the slot number history: getting it wrong caused writes into
freed blocks).

## If far lights are the wrong colour

1. Press F9 with `debug = 1`, `log_blocks = 1`, `log_samples = 50`: the log
   lists blocks with light counts and the first raw entries decoded to
   RGB/HSV with a `match=` flag. A light that should match but shows
   `match=0` is a matcher tuning problem (`hue_window`, `min_saturation`,
   the cream zone keys). One that never appears is a block the hook did
   not see.
2. `stats:` on F9 shows `calls=` (hook invocations) and `lights=` /
   `recolored=`. Zero calls means the pattern did not match: check Cfx's
   `LoadStreamingFile.cpp` for the current pattern after a game update.
3. `repainted N loaded objects ... dropped X stale` (debug) after a
   repaint: many stale drops mean blocks are leaving the registry without
   a `Remove`, i.e. the unload detour is on the wrong slot.
4. The `probe` key logs every LOD light within a radius of a world point as
   its block loads, with the original colour and whether it matched.
