# Development

Building, the source layout, where every fact came from, and the design
rules that keep the plugin alive under FiveM.

## Building

Docker Desktop only; nothing else is installed on the host. The plugin is
a plain Windows x64 DLL cross-compiled from a Debian container with
mingw-w64, CMake and Ninja.

```powershell
.\build.ps1            # -> .\dist\VLights.asi + vlights.ini
.\build.ps1 -Install   # also copies into %LOCALAPPDATA%\FiveM\FiveM.app\plugins
```

or directly:

```
docker build --target export -o dist .
```

The build first runs the colour-math tests (`tests/test_recolor.cpp`)
natively and stops if they fail, then cross-compiles the `.asi` statically
(no `libstdc++`/`libgcc` DLL dependencies; the log prints the import
table). Plain CMake, so Visual Studio works too; MinHook and Dear ImGui are
vendored under `third_party/`, no network needed.

Installing while the game runs works by rename-then-copy (the next launch
picks the new file up). A leftover `FiveM_DumpServer` crash dialog keeps the
file open; close it first.

**Version.** The `VERSION` file at the repo root (`MAJOR.MINOR.PATCH`) is
the only place the version lives. CMake reads it into `project(VERSION)`,
generates `vlights/version.h` (`VLIGHTS_VERSION`, `VLIGHTS_VER_MAJOR`...)
for the code, and fills the Windows version resource (`res/version.rc.in`)
from it; `build.ps1` prints the result. Author and project URL come from
the `VLIGHTS_AUTHOR` / `VLIGHTS_URL` CMake cache variables. CI reads the
same file, and a pull request into main must raise it (see below).

## CI and releases (GitHub Actions)

- `.github/workflows/ci.yml` runs on every pull request and on pushes to
  any branch but main: the same Docker build, tests included, with the
  `.asi` and ini uploaded as a run artifact. To make it a merge gate, add
  a branch protection rule for `main` on GitHub (Settings > Branches)
  that requires the `build` and `version bump` status checks; PRs then
  cannot merge until both pass.
- `.github/workflows/release.yml` runs on every push to main: builds,
  reads `VERSION`, and if no `v<version>`
  tag exists yet creates the tag and a GitHub Release with `VLights.asi`
  and `vlights.ini` attached and the commit subjects since the previous
  tag as notes. A push to main without a version bump builds and uploads
  the artifact but publishes nothing. The CI workflow's `version bump`
  check fails any pull request into main whose `VERSION` is not higher
  than main's (or whose tag already exists), so a merge always releases.

The shipped `vlights.ini` is what the plugin writes on first run.
To regenerate it after changing the config keys, load the built `.asi`
from PowerShell in an empty folder and copy the file it writes:

```powershell
[System.Runtime.InteropServices.NativeLibrary]::Load("$pwd\VLights.asi")
```

(That call is also the quickest way to check whether Smart App Control
will let the build load.)

## Layout

```
src/main.cpp            DllMain, startup, hotkey/worker thread
src/plugin/             config state + ini (config.*), logger (log.*),
                        the interface hooks and menu share (plugin.h/.cpp),
                        the release check (update.*: one WinHTTP GET of
                        api.github.com/repos/<owner>/<repo>/releases/latest)
src/color/recolor.h     colour math (pure, header-only, unit-tested)
src/hook/pattern.*      byte-pattern scanner, game build + vtable shift
res/version.h.in        template for the generated version header
src/game/structs.h      RAGE struct layouts + static_asserts, Cfx's byte pattern
src/game/lod_lights.*   the ymap FinishLoading detour: distant / LOD lights,
                        entity light overrides, the probe
src/game/near_lights.*  model lights (store load-complete detour), archetype
                        extensions, store discovery, entity-light helpers
src/game/textures.*     lantern glow: placement detour, raw resource walk,
                        pixel pass, live GPU rebuild
src/game/track.*        live-repaint registry: per-store Remove detours + pool liveness
src/ui/menu.cpp         the menu: own top-level window + D3D11 + ImGui, camera lock
res/VLights.rc          FX_ASI_BUILD resources (one per supported game build)
res/version.rc.in       Windows version info, filled from VERSION
VERSION                 the version, MAJOR.MINOR.PATCH (see CONTRIBUTING.md)
tests/                  host-native tests, run during the Docker build
docs/                   these notes
tools/                  crash-dump symboliser, hang-dump script
third_party/minhook     MinHook v1.3.4 + marked patches (quirks.md)
third_party/imgui       Dear ImGui v1.92.9b (core + dx11/win32 backends)
cmake/                  mingw-w64 toolchain file
Dockerfile, build.ps1   the build
```

## Design rules

- **Every game detour is a `.text` code detour** on a function whose only
  caller is one thread, installed from that thread or while it is
  provably not running: the map hook and the archetype hook from `DllMain`
  (the main thread is blocked in `LoadLibrary`), the store `Remove`,
  load-complete and placement detours from inside the first map hook call.
  MinHook never suspends threads here (quirks.md).
- **Never write to a vtable**, never hook `Present`, never call the
  immediate Direct3D context. The menu is its own window with its own
  device; live texture repaint creates objects on the game's device and
  swaps pointers in heap objects only.
- **Hook order with Cfx.** FiveM already detours `FinishLoading`; MinHook
  detours the detour, so this plugin runs first, then Cfx's fix-up, then
  the game. The plugin clamps to real array counts and never relies on
  Cfx's fix-up.
- **Recolour before the game reads.** LOD arrays before `FinishLoading`'s
  original, model lights before `SetResource`'s original, textures before
  `PlaceResource`'s original. The game copies at those points.
- **Load-time code may trust the object it is handed; repaint-time code may
  not.** Probe pointers, check the store's pool, drop entries that fail.
- **Log what a future debugging session will need**, behind `debug`, and
  keep the always-on log to startup, hook installation, F9 and failures.
- **A fork of the FiveM client is not the way to ship this.** Its CI clones
  private repos, the anti-cheat component is private (public builds get a
  stub), and the build is Windows-only with a large toolchain.

## Verified facts and where they come from

| Fact | Source |
| --- | --- |
| `CDistantLODLight` layout: vtable, `positions` +8, `rgbi` +24, `numStreetLights` +40, `category` +42, 48 bytes | Cfx `code/components/gta-streaming-five/include/EntitySystem.h` |
| `CMapData::distantLodLights` at +392, `name` at +8, `entities` at +96 | same header (Cfx's own `// +392` comment) |
| Byte pattern for `fwMapDataStore::FinishLoading`: `25 00 0C 00 00 3D 00 08 00 00 49 8B 06`, function at match `-0x6F` | Cfx `code/components/gta-streaming-five/src/LoadStreamingFile.cpp`, the `MH_CreateHook(hook::get_pattern(...))` line. Cfx maintains it across builds. |
| `rgbi` entries are `0xIIRRGGBB` | In-game: vanilla sodium decodes as `0xAAFF780A` = RGB (255,120,10), intensity 170; recolouring on that assumption gives the expected colour. |
| `strStreamingModule` virtual order and the six-slot shift on builds >= 2802 | Cfx `Streaming.h` and `code/client/shared/XBRVirtual.h` (`XBR_VIRTUAL_BASE_2802(0)` = `Base<..., 2802, 0, 6>`) |
| Streaming manager pattern `74 1A 8B 15 ? ? ? ? 48 8D 0D ? ? ? ? 41` (+11), `moduleMgr` at manager+144 | Cfx `Streaming.cpp` / `Streaming.h` (`static_assert` on 144) |
| `.asi` files load from `FiveM.app/plugins/` unless `sv_pureLevel >= 2`; `sv_scriptHookAllowed` only gates Script Hook V natives | Cfx `code/components/asi-five/src/Component.cpp`, `code/components/scripthookv/src/VishCompat.cpp` |
| An `.asi` must carry an `FX_ASI_BUILD` resource typed with the numeric game build (2189+) | same `Component.cpp` (`FindResource(hModule, L"FX_ASI_BUILD", MAKEINTRESOURCE(gameBuild))`) |
| `grcTexture`: `ID3D11Resource*` at +0x38, `ID3D11ShaderResourceView*` at +0x78 | Cfx `code/components/rage-graphics-five/include/grcTexture.h` (pad 48 after the vtable, pad 56 between) |
| Model and light-attribute layouts (drawable, fragment, dictionary, 168-byte `CLightAttr`, 160-byte `CLightAttrDef`, 128-byte `fwEntityDef`) | CodeWalker's resource parsers, then confirmed in-game: names read back through the name pointers, `prop_streetlight_01` decodes to its known sodium spot light, probe entities land on the user's coordinates. |
| `CExtensionDefLightEffect` type hash `0x27922C43` | joaat of the name (CodeWalker's MetaNames), confirmed by the extensions found on real lamp entities |
| Raw resource block map and pointer encoding | Read off a live `PlaceResource` call (docs/textures.md) and consistent with Cfx `rage-formats-x/include/pgBase.h` |
| `numStreetLights` cannot be used as a loop bound | CodeWalker's LOD-light generator hardcodes `isStreetLight = false; //TODO: fix this!`, so custom packs ship with `numStreetLights = 0`. The plugin walks the whole array and matches by colour. |

`static_assert`s in `src/game/structs.h` pin the ymap offsets, so a layout
typo is a build error rather than a bad memory write.

## Menu window

The menu is a small always-on-top window with its own D3D11 device and
swapchain, not an overlay drawn inside the game. An earlier build detoured
`IDXGISwapChain::Present` and subclassed the game window to draw ImGui in
the frame, the way FiveM's console does; the anti-cheat terminated the game
about a minute after start, every time (quirks.md). The window hooks
nothing of the game's. The camera is locked while it is open by removing
the game's raw-input mouse registration and restoring it on close; the
cursor is drawn by ImGui so it is always visible.

## First run after a game update

1. If there is no log at all, FiveM's own log in `FiveM.app\logs\` will say
   `does not claim to support game build N`: add
   `FX_ASI_BUILD N BEGIN "\0" END` to `res/VLights.rc` and rebuild.
2. If the log says the pattern matched 0 or more than 1 times, take the
   current pattern from Cfx's `LoadStreamingFile.cpp` (search for
   `fwMapDataStore__FinishLoading`) and update `kFinishLoadingPattern` /
   `kFinishLoadingOffset` in `src/game/structs.h`.
3. With `debug = 1`, `log_samples = 40`: orange lights should decode as
   `rgb=(255,147,41)`-like values with `match=1`. R and B swapped means the
   packing changed: swap the shifts in `Unpack`/`Pack` in
   `src/color/recolor.h`.
4. Check the startup lines listed in diagnostics.md: every store hook
   should report `detoured`, and `unload detour: fired` on F9 should climb
   as you move. A store whose `PlaceResource` "could not be resolved" means
   FiveM changed its handler shape (textures.md).
