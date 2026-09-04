# VLights: guidance for coding agents

Shared, committed, tool-agnostic. Personal settings go in `CLAUDE.local.md`
(gitignored), which `CLAUDE.md` imports after this file.

## What this is

A FiveM `.asi` plugin (Windows x64 DLL, C++17) that recolours GTA V street
lights by detouring the game's streaming code. Read `docs/README.md` first;
each tier of light has its own document, `docs/quirks.md` lists what has
already crashed the game, and `CONTRIBUTING.md` has the versioning and
release rules.

## How to work here

- Build with Docker only: `.\build.ps1` (or `docker build --target export
  -o dist .`). Nothing is installed on the host. The build runs the tests
  first.
- The version lives in `VERSION` and nowhere else. Bump it for any change
  to the plugin; docs-only changes leave it alone.
- Never add attribution trailers, tool advertisements, or generated-by
  notes to commits, pull requests, or code.
- Game hooks follow the rules in `CONTRIBUTING.md` ("Rules for game code")
  and `docs/development.md` ("Design rules"): `.text` detours only, no
  vtable writes, no `Present` hook, no immediate Direct3D context, vtable
  slots through `StreamingVtableShift()`, probe before touching anything
  from a background thread.
- When something is the wrong colour, do not guess at colours: use the
  diagnostics (`debug`, `near_log`, `probe`) described in
  `docs/diagnostics.md` and read the log.
- Check FiveM's public source (citizenfx/fivem: `Streaming.h`,
  `XBRVirtual.h`, `grcTexture.h`, `LoadStreamingFile.cpp`) before adding a
  layout assumption; most past bugs were layouts that were already
  documented there.
- Update the docs in the same change: ini keys go in the template in
  `src/plugin/config.cpp` and, if users care, the README table; regenerate
  the shipped `vlights.ini` afterwards (`docs/development.md`).
