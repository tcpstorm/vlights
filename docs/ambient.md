# Baked road glow (the timecycle's artificial ambient)

With every lamp, LOD light and lantern lens white, the roads under them
were still orange from a distance: a soft wash around each lamp post on
the tarmac, pavements and nearby facades, strongest on the far LOD meshes
where no light reaches at all. It is not a light and not a texture.

## What it is

Map meshes carry a vertex-painted "artificial ambient" weight next to the
natural ambient occlusion. In the deferred ambient pass the game lights
that weight with a colour from the timecycle: `light_artificial_ext_down`
and `light_artificial_ext_up` (`_col_r/g/b` plus `_intensity`) outdoors,
`light_artificial_int_*` inside interiors. Artists painted it around every
lamp post, and the weather timecycles (`common.rpf/data/timecycle/w_*.xml`,
regions `GLOBAL` and `URBAN`, 13 keyframes over the day) give it a sodium
orange at night. That is the orange left on the ground once the lights are
done. The community's white street light packs fix it the same way, by
editing those variables in every weather file (the "Distant LOD Light
Editor" notes on gta5-mods say so explicitly).

The plugin cannot edit the keyframes (no public layout for the cycle data),
so it does what a script would: it registers a timecycle *modifier* and
puts it in the game's extra script slot.

## Where it lives

All from Cfx `code/components/gta-game-five/include/Timecycle.h` and
`src/Timecycle.cpp`, which FiveM's own timecycle editor and natives use:

```
rage::tcManager (static; "48 C1 E0 03 41 80 E1 01 C6 44 24 20 00" +19, lea rip)
  +8    atArray<float>       modifierStrengths
  +0x40 atArray<tcModifier*> modifiers        index = what the script slots hold
  +0x50 atBinaryMap          modifiersMap     { bool sorted; atArray<{u32 hash; tcModifier*}> at +0x58 }

rage::tcModifier (40 bytes)
  +0    atArray<tcModData>   modData          (ptr, u16 count, u16 capacity)
  +16   u32 nameHash   +24 void* varMap (null for created ones)   +32 u32 userFlags

rage::tcModData (12 bytes)   int index; float value1; float value2;
rage::tcVarInfo (32 bytes)   int index; char* name (+8); float default (+16); ...
  table: "7E 6E 45 33 F6" (numVars from the mov at -6, the table pointer from the one at +5)

script data ("48 89 7B 10 8B 05 ? ? ? ? 83 F8 FF 74 13" +6)
  +0 primary index  +4 strength  +8 transition index  +12 strength  +16 speed
  +44 extra index   (SET_EXTRA_TCMODIFIER / CLEAR_EXTRA_TCMODIFIER / GET_EXTRA_TCMODIFIER)

allocator: rage::sysMemAllocator at exe-TLS + offset ("B9 ? ? ? ? 48 8B 0C 01 45 33 C9 49 8B D2 48" +1),
  XBR_VIRTUAL_BASE_2802(2): Allocate = slot 2 + shift, Free = slot 4 + shift (8 and 10 on b3751)
```

Variables are found by name in the `tcVarInfo` table at runtime, never by
number. The modifier value convention comes from the game's own files:
every `*_col_*` entry in `timecycle_mods_*.xml` is `value 1.000`, every
`*_intensity` entry `value 0.000`; the plugin writes colour entries with
`1.0` as the second value and leaves the intensities alone, so the
timecycle's own day/night and weather curve still applies.

## What the plugin does

`src/game/ambient.cpp`:

1. `Install` (DllMain, after the map hook): the four patterns above,
   resolved and checked to lie inside the game image. Reads only.
2. `OnMapHook` (main thread, inside `FinishLoading`, so the thread that
   runs the timecycle update): on the first block the variable table is
   validated (`index == i` for every entry, names readable), the six
   `light_artificial_ext_*_col_*` indices and the six `_int_` ones are
   looked up, the modifier list and script data are range-checked, and the
   allocator's `Allocate`/`Free` are confirmed to point into the game
   image. Then a `tcModifier` named `vlights_ambient` is allocated with
   the game's allocator (capacity 12 entries), appended to `modifiers`
   (the list is grown game-style if full: allocate, copy, free the old
   buffer), to `modifierStrengths` if that array is parallel, and inserted
   in sorted position in the name map. The extra slot is set to its index.
3. `Apply` (any thread, from `ReapplyAll`): the entries are rewritten for
   the current target (entries first, count last, so the game sees either
   set whole) and the slot is claimed or released. `enabled = 0` or
   `ambient = 0` puts `-1` back if the slot is ours.
4. `Tick` (worker, 100 ms): if the modifier moved in the list the slot is
   updated; if the list no longer holds it (manager torn down on a
   session change) the slot is cleared and the next map block registers
   again. If the slot is `-1` while the feature is on it is reclaimed
   (`reclaimed N times` on F9); if a server script holds it with another
   modifier the plugin backs off, logs once, and the menu says "paused".

The colour is `AmbientColour` in `recolor.h`: the target's hue and
saturation at full value times `ambient_brightness`. A white target gives
(1,1,1), the vanilla intensity keyframes still shape it.

## Settings

| Key | Default | |
| --- | --- | --- |
| `ambient` | `1` | Register the modifier and hold the extra slot. |
| `ambient_interior` | `0` | Also the six `light_artificial_int_*` colours (tunnels, car parks, shop interiors). |
| `ambient_brightness` | `1.0` | Scale on the colour, 0 to 4. |

## If the ground is still orange

1. F9: the `ambient:` line. `active` with a modifier number means the
   game is applying it. `paused: a script holds the extra timecycle slot`
   is a server script; the plugin will reclaim the slot when it is
   cleared. `unavailable: ...` names the check that failed.
2. The startup lines: `ambient: timecycle statics: ...` (patterns found),
   `ambient: N timecycle variables; ext colour vars at ...` (names found,
   list and script data plausible), `ambient: loaded modifier #K ... sets
   var V to (value, 1.000)` (the convention holds on this install), and
   `ambient: modifier 'vlights_ambient' ... registered as #N`.
3. A pattern that matched 0 or 2+ times after a game update: take the
   current one from Cfx `Timecycle.cpp` (`hook::get_pattern` lines for
   `ms_numVars`, `ms_Instance`, `ms_scriptData`) or `sysAllocator.cpp`
   (the TLS offset) and update the constants at the top of `ambient.cpp`.
4. Orange only in interiors: `ambient_interior = 1`.
5. Orange only on far trees or vegetation: those meshes do not take the
   artificial ambient (a known timecycle limitation), nothing to do here.
