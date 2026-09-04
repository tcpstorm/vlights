# Near lights (what a lamp actually emits)

Within entity range a lamp is lit by the game's own light entities, created
when the lamp spawns. They are built from one of three definitions, and a
lamp can be orange because of any of them while the others are already
fixed. This is the tier that took the longest to get right, entirely
because the third source was unknown.

## Source 1: model light attributes (`CLightAttr`)

Baked into the model resource (`.ydr` drawable, `.yft` fragment, `.ydd`
drawable dictionary).

```
gtaDrawable            +0xA8 name ptr, +0xB0 CLightAttr* lights, +0xB8 count
gtaFragType            +0x58 name ptr, +0x30 drawable ptr,
                       +0x38 extra drawables ptr array, +0x48 count,
                       +0x110 CLightAttr* lights, +0x118 count,
                       +0xF0 PhysicsLODGroup -> +0x10/+0x18/+0x20 LOD ptrs
                         -> FragPhysicsLOD (304 B): +0xD0 children ptr, +0x11D count
                           -> child: +0xA0 Drawable1, +0xA8 Drawable2
pgDictionary<T>        +0x30 entries ptr, +0x38 u16 count
CLightAttr (168 bytes) +24 r,g,b   +27 flashiness   +28 intensity (float)
                       +38 type    +84 volume colour r,g,b
```

The freeway double-arm lamps (`prop_streetlight_09`) keep their lights in
the fragment's physics children drawables, which is why the walker goes
through `PhysicsLODGroup`. Lights with a flashiness setting are skipped.

Hook: each model store's load-complete virtual, which is
`strStreamingModule::SetResource` (slot 13 = base 7 + the 2802 shift),
found from a stack trace inside the ymap hook as `call [rax+0x68]`. The
three model stores share one 22-byte stub there, so one detour with a
per-store dispatch covers all of them. The placed object is at +0 of the
store's pool entry (`atPoolBase` at store+56). Recolouring happens
**before** calling the original, because the game copies colours when it
creates light entities.

Store discovery: the streaming manager from Cfx's `Streaming.cpp` pattern
(`74 1A 8B 15 ? ? ? ? 48 8D 0D ? ? ? ? 41`, +11, then moduleMgr at
manager+144, module table inline at manager+0x1C0), modules named by the
RAGE class-name string at store+24 (`DrawableStore`, `FragmentStore`,
`DwdStore`, `TxdStore`, `MapDataStore`, `MapTypesStore`), validated by
finding the ymap store the map hook already receives.

## Source 2: entity light overrides in the map block

This was the "still orange" lamp for many rounds. A `.ymap` entity def can
carry a `CExtensionDefLightEffect`, a per-placement set of light
definitions that overrides the model's. Vanilla Los Santos uses these
constantly: one downtown session had 107 blocks carrying them, mostly in
sodium (255,89,7).

```
fwEntityDef (128 bytes)  +8 archetype hash  +12 flags  +16 guid
                         +32 position (3 floats)  +72 parent index
                         +76 lodDist  +80 childLodDist  +84 lodLevel
                         +96 atArray<fwExtensionDef*> extensions (u16 count at +104)
fwExtensionDef           type: vtable slot 7 returns a parStructure*
                         (heap-allocated; name hash at +8);
                         light effect = 0x27922C43 = joaat("CExtensionDefLightEffect")
CExtensionDefLightEffect +32 atArray<CLightAttrDef> instances (u16 count at +40)
CLightAttrDef (160 B)    +20 r,g,b   +23 flashiness   +80 volume colour
```

Handled in the same `FinishLoading` hook as the LOD arrays
(`nearlights::RecolourEntityLights`), before the game instantiates the
entities. Registered for repaint together with the block's `rgbi`
(originals vector = rgbi entries, then colour/volume pairs).

The vtable slot for the type getter is chosen at first use by checking
which candidate returns a plausible descriptor (b2802+ inserted six filler
virtuals; older builds would be slot 1).

## Source 3: archetype extensions (`.ytyp`)

The same extension can sit on an archetype in a `CMapTypes` file
(archetypes atArray at +24, name at +40, `fwArchetypeDef` extensions at
+120). Hooked from `DllMain` through the archetype store's own
`FinishLoading`, found as the sibling call next to the ymap one. A survey
of vanilla archetype files found zero light-effect extensions, so this
path exists for completeness and for modded content.

## Why near lights do not repaint live

The game copies `CLightAttr` / `CLightAttrDef` into its runtime light
objects (a light extension on each spawned entity) and reads the copies
from then on. Rewriting the definitions afterwards changes nothing until
the entity spawns again. Reaching the runtime copies would mean walking
the game's entity pools; not done. A colour change therefore applies to
near lamps as they stream back in. The LOD tiers and the textures do
repaint live.

## Matcher

`src/color/recolor.h`, HSV, brightness-invariant. Zone 1 (sodium): hue
within `hue_window` (13) of `source` (255,147,41: hue 29.7), saturation
at least `min_saturation` (0.6). Zone 2 (cream): hue within `hue_window2`
(14) of `source2` (255,227,166: hue 41), saturation between
`min_saturation2` (0.30) and `max_saturation2` (0.70). The ceiling keeps
amber runway edge lights (255,188,2: saturation 0.99) out at the user's
request; the floor keeps cream-white wall lights out. Zone 3 (cool): hue
within `hue_window3` (15) of `source3` (120,255,232: hue 170), saturation
0.30 to 0.80, for `prop_streetlight_01b`'s teal light; the ceiling keeps
cyan neon out. `keep_brightness` scales the target by the light's own value
so a dim lamp stays dim.

**All street lamps.** Vanilla has lamp-post variants whose light is not
sodium at all: `_01b` teal (120,255,232), `_11a/b` and `_14a` near-white
(221,236,231), `_14a` pale blue (197,240,255), `_15a` (234,247,225). With
`all_streetlights = 1` (default) every light on a model whose name contains
one of `streetlight_names` (default `streetlight`) is forced to the target
whatever its colour (flashing lights still skipped), and in the LOD arrays
the entries the block flags as street lights (`numStreetLights`) are forced
too, and so are the per-placement light overrides on entities whose
archetype hash is a known lamp model (`streetlight_models`, plus every
model loaded with a streetlight name). `ForceRecolor` in `recolor.h` is
the matcher-free path; brightness is still kept.

## If a nearby lamp is the wrong colour

1. `debug = 1`, `near_log = 1`, restart, walk to the lamp, F9. The log has
   one line per model (`near: yft slot N 'pack:/prop_...' lights=..
   recolored=.. colours: (r,g,b hH sS)xN ...`) and one per block with
   entity lights (`near: ymap XXXXXXXX entity lights=..`), each with a
   full colour breakdown. F9 also prints `unmatched warm light colours so
   far:` with every warm colour that matched nothing, by RGB and count.
2. If the colour is listed and unmatched, tune the matcher. If it is a
   colour you want excluded, that is what the saturation bounds are for.
3. If nothing warm is listed at all, the light is coming from somewhere
   the plugin does not read. Set `probe = x y 25` to the lamp's
   coordinates: as its block loads, every entity within the radius is
   logged with archetype hash, lodDist, and its extensions, light colours
   included. That line is what exposed source 2. Match the hash against
   the model names in the near log with joaat (lower-case).
4. `near: ... has NO lights reachable` for a lamp-named model means the
   walker did not find its light array: a new fragment layout, or lights
   in a child drawable the walker does not visit yet.
