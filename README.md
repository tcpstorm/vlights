# LOD Light Recolor

A standalone FiveM `.asi` plugin that recolors distant / LOD street lights
(the sodium-orange lamps you see across the city at night) to a
configurable colour, at map-data load time, client-side only. Purely
cosmetic: it rewrites colour bytes in already-loaded map data and changes
nothing else.

Made for servers that stream their own LOD-light ymaps, where a `mods/`
folder replacement loses to the streamed pack. Nothing in it is
server-specific; story mode works the same.

Includes an in-game menu (F10 by default) for tuning colours live, and a
reload hotkey (F9) for the ini. Both repaint every loaded light instantly.

## How it works

1. RAGE parses each `.ymap` into a `CMapData`. The distant LOD lights in it
   are a struct-of-arrays: `positions[]`, `rgbi[]`, `numStreetLights`,
   `category`.
2. The game calls `fwMapDataStore::FinishLoading` on every block right after
   parsing, whether it came from the base game, `mods/`, or a server's
   streamed resource. FiveM already hooks this function itself (to fix a
   `numStreetLights` bug).
3. This plugin detours the same function, walks `rgbi[]`, and rewrites every
   entry whose hue/saturation looks like sodium orange to the target colour
   before the game ever reads it. The mid-range LOD lights take their colour
   from these same entries by index, so both LOD tiers change together.
4. The near-range light of a lamp comes from one of two places, and the
   plugin recolours both in the same load hooks (`near_enabled`, default
   on): the `CLightAttr` array baked into the prop's model, and, for a
   large share of vanilla lamp placements, a per-entity light-effect
   extension stored in the `.ymap` itself that overrides the model's
   lights. See "Nearby lamps" below.

## Nearby lamps (model and entity lights)

Lamps within entity range are lit by the game's own light entities, which
are created when the lamp spawns from one of two definitions:

- the `CLightAttr` array baked into the prop's model (`prop_streetlight_01`
  carries one spot light at (255,104,0), intensity 32);
- a `CExtensionDefLightEffect` on the *entity* inside the `.ymap`, which
  overrides the model's lights for that one placement. Vanilla Los Santos
  uses these a lot: one downtown session saw 107 map blocks carrying them,
  most in sodium orange (255,89,7). This is why a lamp can stay orange even
  though its model was recoloured. (The same extension can sit on
  archetypes in `.ytyp` files; the plugin handles that too, but no vanilla
  archetype has one.)

With `near_enabled = 1` (default) the plugin recolours both with the same
matcher and target, before the game copies them into its light entities, so
near and far tiers agree.

**Near lights do not repaint live.** The game copies the definitions when
it spawns the lamp and reads the copies from then on. A colour change from
the menu or ini applies to the LOD tiers instantly, and to near lamps as
they stream in again (leave the area and come back, or reload). The LOD
tiers repaint live because the game reads those arrays in place.

Entity light effects are read in the ymap hook itself: `fwEntityDef`
(128 bytes: archetype hash +8, position +32, extensions `atArray` +96),
extension type via the `parStructure` returned by vtable slot 7 (name hash
`0x27922C43` = `joaat("CExtensionDefLightEffect")`), instances at +32 as
160-byte `CLightAttrDef` (colour +20, flashiness +23, volume colour +80).

Model lights (`src/game/near_lights.cpp`), every step checked at runtime
against the ymap store the map hook already receives:

1. The streaming manager comes from a pattern Cfx maintains in its own
   `Streaming.cpp`; its module table is found by searching the manager for
   the known ymap store (24 modules, inline at manager+0x1C0 on b3751).
2. Modules are named by their RAGE class name at store+24 ("MapDataStore",
   "DrawableStore", "FragmentStore", "DwdStore"); that is how the `ydr`,
   `yft` and `ydd` stores are picked. There is no extension string.
3. The streaming engine completes every load with `call [vtable+0x68]`
   (slot 13, args: store, slot index), read off a stack trace inside the
   ymap hook. On the model stores that slot is one shared 22-byte stub
   (`sub rsp,28h; mov rax,[rcx]; mov r8,[r8+8]; call [rax+158h]; ...`).
   It is detoured by code with a thunk that passes all argument registers
   and the return value straight through and only acts when the store is
   one of the three.
4. The placed model is the first field of the store's pool entry
   (`atPoolBase` at store+56; entry name hash at +12). Its vtable is in the
   game image and its name pointer (drawable +0xA8, fragment +0x58) reads
   e.g. `pack:/prop_streetlight_01`.
5. Light arrays: drawable +0xB0/+0xB8, fragment +0x110/+0x118 plus its
   drawable(s), dictionary +0x30/+0x38; each `CLightAttr` is 168 bytes with
   colour at +24 and volume colour at +84. Lights with a flashiness
   setting are left alone.

**Never call vtable slot 8 on the model stores expecting a read-only
`GetPtr`.** It is not one there: it wrote through a null array at the slot
index and crashed the game (three identical `CInstanceListAssetLoader::Init`
crashes, dump analysed). Slots 5 (`Load`) and 7 (`SetResource`) are shared
base-class stubs used by every store during session init; hooking them
crashed the game too. Live repaint covers model lights through the same
registry as the LOD blocks.

## Verified facts and where they come from

Everything the plugin depends on was checked against public source, not
guessed.

| Fact | Source |
| --- | --- |
| `CDistantLODLight` layout: vtable, `positions` +8, `rgbi` +24, `numStreetLights` +40, `category` +42, 48 bytes | citizenfx/fivem `code/components/gta-streaming-five/include/EntitySystem.h` |
| `CMapData::distantLodLights` at +392, `name` at +8 | same FiveM header (Cfx's own `// +392` comment) |
| Byte pattern for `fwMapDataStore::FinishLoading`: `25 00 0C 00 00 3D 00 08 00 00 49 8B 06`, function starts at match `-0x6F` | citizenfx/fivem `code/components/gta-streaming-five/src/LoadStreamingFile.cpp`, the `MH_CreateHook(hook::get_pattern(...))` line. Cfx maintains this across game builds. |
| `rgbi` entries are `0xIIRRGGBB` (intensity in the top byte) | Confirmed in-game: vanilla sodium street lights decode as e.g. `0xAAFF780A` = RGB (255,120,10), intensity 170, and recolouring on that assumption produces the expected colour. |
| `.asi` files in `FiveM.app/plugins/` are loaded at game load unless the server's `sv_pureLevel` is 2 or higher. `sv_scriptHookAllowed` only gates Script Hook V natives, which this plugin never uses. | citizenfx/fivem `code/components/asi-five/src/Component.cpp`, `code/components/scripthookv/src/VishCompat.cpp` |
| FiveM refuses any `.asi` on game build 2189+ unless it carries a resource named `FX_ASI_BUILD` whose type is the numeric game build. `src/LodLightRecolor.rc` declares one entry per build FiveM currently ships; add a line when a new build appears (FiveM's own log tells you the number). | citizenfx/fivem `code/components/asi-five/src/Component.cpp` (`FindResource(hModule, L"FX_ASI_BUILD", MAKEINTRESOURCE(gameBuild))`) |
| Model and light-attribute layouts (drawable, fragment, dictionary, 168-byte `CLightAttr`) | First taken from CodeWalker's resource parsers, then confirmed in-game: model names read back through the name pointers, and `prop_streetlight_01` decodes to its known sodium spot light. |
| `numStreetLights` cannot be used as the loop bound: CodeWalker's LOD-light generator hardcodes `isStreetLight = false; //TODO: fix this!`, so custom packs made with it ship with `numStreetLights = 0`. The plugin iterates the whole `rgbi` array and matches by colour instead. | CodeWalker `Project/Panels/GenerateLODLightsPanel.cs` |

Compile-time `static_assert`s in `src/GameStructs.h` pin every offset above,
so a layout typo is a build error rather than a bad memory write.

## Building (Docker, nothing installed locally)

The plugin is a plain Windows x64 DLL, so it cross-compiles from a Linux
container with mingw-w64. Only Docker Desktop is needed on the host.

```powershell
.\build.ps1            # -> .\dist\LodLightRecolor.asi + lodlight_recolor.ini
.\build.ps1 -Install   # also copies into %LOCALAPPDATA%\FiveM\FiveM.app\plugins
```

or directly:

```
docker build --target export -o dist .
```

The build runs the colour-math unit tests (`tests/test_recolor.cpp`) natively
first and fails if they do, then cross-compiles the `.asi` statically (no
`libstdc++`/`libgcc` DLL dependencies; the build log prints the DLL's imports
so you can see that).

It also builds with Visual Studio if you ever want that: plain CMake, and
MinHook is vendored under `third_party/minhook` so no network is needed.

## Installing

Copy `LodLightRecolor.asi` into `FiveM.app\plugins\` (create the folder if
it does not exist). The `.ini` is optional: the plugin writes a default one
next to itself on first run.

Before investing time, confirm the server allows plugins. Its public
`info.json` (`http://<server>:30120/info.json`, under `vars`) reports
`sv_pureLevel`. Anything below 2 is fine.

Whether a given server's *rules* permit a cosmetic client plugin is your
call, not the plugin's.

**Windows Smart App Control** (Windows 11) evaluates unsigned DLLs against
a cloud reputation service per file. A freshly built `.asi` can be blocked
on its first load ("Couldn't load LodLightRecolor.asi"; Code Integrity
event 3077) and allowed a minute later. `build.ps1` cannot fix that; either
relaunch once, or turn Smart App Control off (one-way switch). You can
check a build's verdict without the game by loading it from PowerShell:
`[System.Runtime.InteropServices.NativeLibrary]::Load("path\LodLightRecolor.asi")`.

## Configuration (`lodlight_recolor.ini`)

Lives next to the `.asi`. Press the reload key in-game (default F9) to re-read
it. Every loaded block is repainted immediately from its remembered original
colours, so you can tune by eye. `enabled = 0` plus F9 puts the originals
back live.

| Key | Default | Meaning |
| --- | --- | --- |
| `enabled` | `1` | Master switch. |
| `source` | `255 147 41` | Colour being hunted. Only its hue matters. `R G B` or `#RRGGBB`. |
| `target` | `235 240 255` | What matching lights become. |
| `hue_window` | `13` | Degrees either side of `source`'s hue that still count as a match. Sodium lamps decode to hue 17-33 (`255 104 0`, `255 120 10`, `255 162 52`). |
| `min_saturation` | `0.6` | Lights below this saturation are skipped in the sodium zone. Sodium is 0.75-1.0. |
| `match_cream` | `1` | Second match zone for the cream freeway lamps (`prop_streetlight_06`/`_08`, light colour `255 227 166`: hue 41, saturation 0.35). |
| `source2` | `255 227 166` | Reference colour of that zone (hue only). |
| `hue_window2` | `14` | Hue window of the cream zone: covers the cream freeway lamps (hue 41) and the pale-yellow street lights baked into downtown map pieces such as `dt1_21_ground1_decals` (hue 48-55). |
| `min_saturation2` / `max_saturation2` | `0.30` / `0.70` | Saturation band of the cream zone. The ceiling keeps amber runway edge lights (0.99) and car-park lights (0.93) out; the floor keeps cream-white wall lights (0.07-0.21) out. |
| `blend` | `1.0` | 1 = replace with `target`, 0.5 = halfway, 0 = untouched. |
| `keep_brightness` | `1` | Scale `target` so each light keeps its own brightness instead of every light becoming identical. |
| `debug` | `0` | Master switch for every per-object log line (`log_samples`, `log_blocks`, `near_log`, first-call traces). Off, the plugin writes nothing during play. On, it costs frame time while models stream in. |
| `log_samples` | `0` | Log the first N raw entries seen (raw hex, decoded RGB, HSV, position, match). |
| `log_blocks` | `0` | One log line per map block with counts. |
| `reload_key` | `F9` | `F1`..`F24`, or a hex/decimal virtual-key code. `0` disables. |
| `menu_key` | `F10` | Toggles the menu window. `0` disables it entirely. |
| `live_repaint` | `1` | Repaint already-loaded blocks on reload/menu changes. `0` = changes only affect blocks that stream in later. |
| `near_enabled` | `1` | Also recolour model (near-tier) lights. Hooks install at game start; changing this needs a restart. |
| `near_log` | `0` | Log one line per placed model or map block that has lights, with a breakdown of its light colours. F9 also prints every warm colour that matched nothing so far. |
| `probe` | `0 0 25` | `x y radius`. With `debug` on, every block that streams in is scanned and the entities and LOD lights within `radius` of that world point are logged (model hash, extensions with their light colours, original colour). This is how the entity-light tier was found. `0 0` = off. |

Matching is done in hue/saturation rather than RGB distance so it is
brightness-invariant: a dim sodium light and a bright one both match, while
yellow traffic lights, red brake lights and white LEDs do not. The second
zone exists because the freeway overhead lamps use a cream light that is
neither sodium nor white; it is matched by a narrow hue window with a
saturation ceiling.

## Menu window

Press F10. The menu opens as a small always-on-top window over the game
(FiveM runs borderless by default, so it simply floats on top). It has the
target colour with presets (cool LED, neutral, warm LED, and a garish green
for checking coverage), blend, keep-brightness, the source colour and match
thresholds, live counts, and Save / Reload buttons for the ini. Every change
repaints all loaded LOD lights on the spot. F10 again, or Close, hides it.

Why a window and not an in-game overlay: an earlier build detoured
`IDXGISwapChain::Present` and subclassed the game window to draw ImGui
inside the game, the way FiveM's own console does. FiveM's anti-cheat
(adhesive) terminated the game about a minute after start, every time, with
the "Early-exit trap" dialog; the crash dump shows adhesive on the main
thread right before the trap. The window approach (`src/Overlay.cpp`) uses
its own D3D11 device and swapchain and hooks nothing of the game's, so
there is nothing for an integrity check to find. The trade-off is that
clicking the window takes focus from the game while you use it.

## Verified in-game (story mode, build 3751)

Hook fires on every map block, ~51k lights walked in one session, ~25%
recolored. Vanilla sodium decodes as `0xAAFF780A`-style values (intensity
170, RGB `255 120 10`), confirming `0xIIRRGGBB`. A green diagnostic target
was visible across the whole distant city. Near lamps: model lights and
ymap entity light effects both confirmed recoloured at the lamp (Little
Seoul, `prop_streetlight_03d` with an entity light effect at (255,89,7)).

## First run: verify the byte order

1. Set `debug = 1`, `log_samples = 40` and `log_blocks = 1`, join the server at night, stand
   somewhere with clearly orange street lights in the distance.
2. Open `lodlight_recolor.log` next to the `.asi`. You should see
   `hook installed`, then sample lines like
   `raw=0x??FF9329 rgb=(255,147,41) ... match=1` for orange lights.
   If orange lights decode with R and B swapped (e.g. `rgb=(41,147,255)`),
   the packing assumption is wrong: swap the shifts in `Unpack`/`Pack` in
   `src/Recolor.h` and rebuild. That is the entire blast radius.
3. Set `debug` back to `0`.

If there is no log at all, check FiveM's own log in `FiveM.app\logs\` for
`Unable to load ... does not claim to support game build N`: add
`FX_ASI_BUILD N BEGIN "\0" END` to `src/LodLightRecolor.rc` and rebuild.

If the log says the pattern matched 0 or more than 1 times, the game build
changed. Take the current pattern from Cfx's `LoadStreamingFile.cpp` (search
for `fwMapDataStore__FinishLoadingHook`) and update `kFinishLoadingPattern`
and `kFinishLoadingOffset` in `src/GameStructs.h`.

## Caveats

- **Hook order.** Cfx has already detoured `FinishLoading` when this plugin
  loads, so MinHook detours the detour: this plugin runs first, then Cfx's
  hook, then the original. The plugin clamps to the real array count and
  never relies on Cfx's fix-up.
- **All game detours are on functions whose only caller is the main thread**, installed from that thread: the map load hook from `DllMain`, the store `Remove` hooks and the model load-complete hook from inside the map hook. MinHook never suspends threads here and writes its jump with one aligned 8-byte store.
- **Hooks are installed inside `DllMain`, on purpose.** FiveM loads plugins
  from the main thread (its log tags the loader lines `MainThrd`), and the
  main thread is the only caller of `fwMapDataStore::FinishLoading`. While
  we are inside `DllMain` that thread is blocked in `LoadLibrary`, so the
  target cannot be executing while its first bytes are overwritten. Only the
  reload-hotkey loop runs on a worker thread.
- **Live repaint never writes to a vtable.** An earlier build patched
  `CMapData`'s destructor slot in the game's read-only data; FiveM's
  anti-cheat (adhesive) terminated the game about eight seconds later with an
  "Early-exit trap" (the crash dump shows adhesive on the main thread right
  before the trap). The current build instead detours the code of
  `strStreamingModule::Remove` (address read from the ymap store's vtable,
  slot 3 per Cfx's `Streaming.h`), the same kind of `.text` detour as the
  load hook, which adhesive tolerates. Before every repaint write the block
  is also checked read-only against the store's asset pool (`atPoolBase` at
  store+56), so a freed slot can never be touched. `live_repaint = 0`
  disables all of it.
- **MinHook is patched to never suspend threads.** Stock MinHook
  enumerates threads with `CreateToolhelp32Snapshot` to suspend them while
  it writes the jump; FiveM stubs that call out (it returns failure with no
  error code), which stock MinHook reports as `MH_ERROR_MEMORY_ALLOC` and
  refuses to hook. The vendored copy skips suspension entirely by default
  (`mh_lodlight_allow_suspend`), which is safe here because every detour is
  installed from the only thread that calls its target: the map load hook
  from `DllMain` on the main thread, the store `Remove` hook from inside
  the map load hook. An `NtGetNextThread` fallback exists behind the flag.
- **No render hooks at all.** See "Menu window" above. `menu_key = 0`
  disables the menu window entirely.
- **Memory checks under the anti-cheat.** `VirtualQuery` on heap addresses
  has measured anywhere from 4 microseconds to 1.3 milliseconds per call
  between sessions; a `ReadProcessMemory` self-probe has been under a
  microsecond every time. Both are timed at the first map load and the
  probe is preferred; without either, only vtable-in-image checks remain.
  Startup sweeps never do per-slot syscalls (that hung loading at
  `ambient_SD.ipl`).
- **Live repaint of near lights validates every pointer first.** Blocks are
  walked again from the menu thread long after they loaded; the entity
  array and each entity are probed before use, and a block that fails is
  dropped from the registry (0.15.0 crashed there once).
- **Anti-cheat.** The `plugins/` folder is a first-party Cfx extension point
  and the plugin only touches map data, but a server running its own
  module-scanning anti-cheat could still flag an unknown DLL. Unknown; not
  something that can be verified from source.
- **A fork of the FiveM client is not the way to ship this.** The client's CI
  clones non-public `private` and `closed` repos, the anti-cheat component is
  private (a public build substitutes a stub called `sticky`), and the client
  build is Windows-only with a large toolchain. The `.asi` route needs none
  of that.

## Layout

```
src/main.cpp            DllMain, startup, hotkey thread
src/plugin/             plugin core: config state + ini (config.*), logger (log.*),
                        the interface the hooks and menu share (plugin.h/.cpp)
src/color/recolor.h     colour math (pure, header-only, unit-tested)
src/hook/pattern.*      byte-pattern scanner over the main module
src/game/structs.h      RAGE struct layouts + static_asserts, Cfx's byte pattern
src/game/lod_lights.*   the ymap FinishLoading detour: distant / LOD lights
src/game/near_lights.*  model (near-tier) lights via the store load-complete detour
src/game/track.*        live-repaint registry: per-store Remove detours + pool liveness
src/ui/menu.cpp         the menu: own top-level window + D3D11 + ImGui, camera lock
res/LodLightRecolor.rc  FX_ASI_BUILD resources (one per supported game build)
tests/                  host-native tests, run during the Docker build
third_party/minhook     MinHook v1.3.4 + marked patches (see Caveats)
third_party/imgui       Dear ImGui v1.92.9b (core + dx11/win32 backends)
cmake/                  mingw-w64 toolchain file
Dockerfile              the build
build.ps1               docker build wrapper, optional -Install
```
