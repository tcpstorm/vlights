# Docs

Working notes for the next time a lamp is the wrong colour. Each document
covers one light source the plugin touches, what the game does with it, the
memory layouts and hooks involved, the diagnostics built into the plugin,
and the procedure that found the last problem of that kind. The main
`README.md` is the overview; these are the reference.

| Document | Covers |
| --- | --- |
| [lod-lights.md](lod-lights.md) | Distant and mid-range LOD lights: the `.ymap` arrays, the `FinishLoading` detour, live repaint. |
| [near-lights.md](near-lights.md) | The lights near lamps actually emit: model light attributes, per-placement light overrides in `.ymap` entity defs, archetype extensions, and why these do not repaint live. |
| [textures.md](textures.md) | The lantern glow: emissive textures embedded in models or in `.ytd`, the placement detour, raw-resource walking, DXT endpoint recolouring, live GPU rebuild. |
| [quirks.md](quirks.md) | FiveM and engine behaviour that shaped every design choice: the 2802 vtable shift, the anti-cheat's tolerances, MinHook under FiveM, memory-check cost, unload tracking. |
| [diagnostics.md](diagnostics.md) | The ini switches, what each log line means, the probe, and the crash-dump workflow with a symbol build. |
| [development.md](development.md) | Building, source layout, design rules, the table of every verified fact and its source, the menu window, and what to check after a game update. |

Conventions used throughout: offsets are bytes from the start of the object,
game build 3751 unless stated, "Cfx" means the public citizenfx/fivem
source, and "slot" means a vtable index *after* the six-slot shift that
FiveM documents for builds 2802 and newer (see quirks.md).
