# Diagnostics

Everything the plugin can tell you, and how to read a crash.

## Switches (ini, next to the `.asi`; F9 re-reads it)

| Key | Effect |
| --- | --- |
| `debug = 1` | Master switch. Off, nothing below is written during play. On, it costs frame time while things stream in; turn it off again when done. |
| `near_log = 1` | One line per model with lights, per map block with entity lights, per model's embedded textures, and per selected texture recoloured, each with colour breakdowns. |
| `log_blocks = 1` | One line per map block with LOD light counts. |
| `log_samples = N` | The first N raw LOD entries decoded (raw hex, RGB, HSV, position, `match=`). |
| `probe = x y r` | As each block streams in, log every entity (archetype hash, lodDist, extensions with light colours) and LOD light (original colour, matched or not) within `r` metres of that world point. `0 0` = off. |

Always logged (debug or not): startup lines, hook installation, F9
`stats:` and `textures:` lines, `unload detour:` counts, config
warnings, the `update check:` outcome, and anything that failed.

## Reading the F9 block

```
stats: calls=... blocks_with_lights=... lights=... recolored=... near_models=... near_lights=... near_recolored=... loaded_now=...
unload detour: fired N times, dropped M tracked slots
unmatched warm light colours so far: (r,g,b hH sS)xN ...
textures: P resources placed, D with selected textures, T textures recoloured (B blocks); R registered for live repaint
config (hotkey): ...
```

- `calls` is the map hook. Zero means the byte pattern did not match.
- `unload detour: fired` should climb as you move; if it stays at zero
  the `Remove` slot is wrong for this build (quirks.md).
- `unmatched warm light colours` is the list to look at when something is
  still orange: if the colour is there, it is a matcher setting; if it is
  not, the light comes from a source the plugin does not read.
- `textures: ... 0 with selected textures` after driving past lamps means
  the placement hook did not install; look for the `tex:` lines at start.
- `hook time:` is the cost on the game's threads: per hook, calls, total
  milliseconds, average and worst microseconds. Map blocks and models run
  on the main thread as things stream in; placement on a streaming thread;
  repaint on the worker thread. A session's total in the tens of
  milliseconds is the expected order; a worst case in the milliseconds
  points at something to look at.

## Startup lines worth knowing

```
VLights X.Y.Z starting (thread ..., game build 3751, streaming vtable shift 6)
fwMapDataStore::FinishLoading at ... (base+0x...)
map hook installed (thread ..., threads frozen via: none)
near: memory checks: VirtualQuery N us, ReadProcessMemory probe M us -> probe
near: modules: Archive ? DwdStore ? DrawableStore ? TxdStore ? FragmentStore ...
near: 'ydr' store ...: load-complete vtable[13] at ... detoured
ymap store ...: Remove (vtable slot 9) at ... detoured; pool count=... entrySize=40
tex: txd store ...: PlaceResource vtable[12] -> ... (FiveM_b3751_GTAProcess.exe+0x...) detoured
tex: ydr store ...: PlaceResource shares txd's routine ... (already detoured)
```

A `memory checks` line choosing "vtable checks only" means both syscalls
were slow that session; expect more conservative behaviour, not crashes.

## Crash dumps

FiveM writes a minidump to `%LOCALAPPDATA%\FiveM\FiveM.app\crashes\` and
shows `vlights.asi+OFFSET`. To turn the offset into a function:

1. Build an unstripped copy. Remove ` -s` from the link options in
   `CMakeLists.txt` temporarily and build with `--target build`, then dump
   symbols from inside the image:

   ```
   docker build --target build -t lodsym .
   docker run --rm lodsym sh -c 'x86_64-w64-mingw32-nm -n --demangle /out/VLights.asi' > dist-sym/syms.txt
   ```

   Code layout is unchanged by stripping, so offsets match the shipped
   build as long as the sources are the same.
2. Symbolise the offset against `syms.txt` (the image base in the symbol
   file is 0x2DA410000 for this toolchain; subtract it, add the offset).
3. For the full picture, `tools/dump_analyse.py <dump> <syms.txt>` walks
   every thread's stack and prints the frames that fall inside the plugin
   with their symbol, plus registers of the faulting thread. It needs the
   `minidump` Python package.

The three crashes so far, for pattern-matching: writes into a freed map
block from the repaint thread (wrong `Remove` slot); a background walk
over loaded blocks dereferencing garbage (no probing, NaN distance);
`CollectEntityLights` reading a dead entity list on repaint (validated
since).

## Hangs

A hang during loading at `common:/data/levels/gta5/ambient_SD.ipl` has
twice been a sweep doing a kernel call per pool slot under the anti-cheat.
`tools/` had a `MiniDumpWriteDump` script for a hung process; the stack of
the main thread names the sweep.

## Finding what is at a spot

Get the player's coordinates (the user reads them off the server or a
trainer), set `probe = x y 25`, `debug = 1`, restart, walk there, F9.
Match `archetype=XXXXXXXX` against model names with joaat over the
lower-case name (a few lines of Python), and you have the model; its
lights, entity overrides and embedded textures are then all in the
`near:` lines for that model.
