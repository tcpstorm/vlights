# Contributing

VLights is a FiveM `.asi` that recolours street lights by hooking the
game's streaming code. That makes it a project where a wrong offset
crashes someone's game, so the rules below are about not shipping that.

## Before you start

- Read `docs/README.md` and the document for the tier you are touching.
  `docs/quirks.md` lists what has already crashed the game once; do not
  rediscover it.
- Build with Docker only (`.\build.ps1`); nothing needs installing on the
  host. The build runs the tests first and fails on them.
- Test in-game. FiveM story mode is enough, at night, near street lights.
  A change to a hook or a memory layout is not done until it has survived
  a session of driving and flying around with the menu open.

## Versioning: one file

The version lives in exactly one place: the `VERSION` file at the repo
root, containing `MAJOR.MINOR.PATCH` and nothing else. CMake turns it into
`project(VERSION)`, generates `vlights/version.h` for the code, and fills
the Windows version resource; the release workflow reads it to name the
tag. Never put a version string anywhere else.

To make a release:

1. Bump `VERSION` in your branch. Patch for fixes and diagnostics, minor
   for a new capability (a new light source, a new ini key that changes
   behaviour), major when settings or file names stop being compatible.
2. Open a pull request into `main`. CI runs two checks: `build` (the Docker
   build with tests) and `version bump` (the PR's `VERSION` must be higher
   than main's and its `v<version>` tag must not exist yet). Both are
   required to merge.
3. Merge. The push to `main` builds again and, because the tag is new,
   publishes a GitHub Release with `VLights.asi` and `vlights.ini`
   attached and the commit subjects since the previous tag as notes.

A push to `main` that does not raise `VERSION` builds but releases
nothing, which is why the bump check exists on pull requests: every merge
that changes the plugin is a release, and every release corresponds to
one version. The bump check only applies when the plugin itself changes:
anything under `src/`, `res/`, `tests/`, `third_party/`, `cmake/`, or the
build files (`CMakeLists.txt`, `Dockerfile`, `build.ps1`, `vlights.ini`,
`VERSION`). Pull requests that touch only docs, media, `tools/`, the
workflows, or the licence are exempt and merge without a release.

## Pull requests

- One topic per PR. A rename, a hook change and a doc fix are three PRs.
- Commit messages describe the change and why; no attribution trailers or
  tool advertisements.
- If you touched a memory layout, a vtable slot, a byte pattern, or a hook
  installation, say in the PR which game build you tested on and paste the
  startup lines from `vlights.log` (they list every hook and where it
  landed).
- Update the docs in the same PR. New ini keys go in the ini template
  (`src/plugin/config.cpp`), the README table if a user would care, and
  `docs/diagnostics.md` if they are diagnostic.
- Regenerate the shipped `vlights.ini` when config keys change
  (`docs/development.md` shows how).

## Rules for game code

These are not preferences; each one is backed by a crash in `docs/quirks.md`.

- Every detour is a `.text` code detour on a function whose only caller is
  one thread, installed from that thread or from `DllMain`. No vtable
  writes, no `Present` hook, no immediate Direct3D context calls.
- Look up vtable slots through `StreamingVtableShift()`; the base numbers
  in Cfx's `Streaming.h` are six too low on builds 2802 and newer.
- Recolour before calling the original. The game copies at load.
- Load-time hooks may trust the object they were handed. Repaint-time code
  may not: probe every pointer with the kernel probe, check the store's
  pool, drop what fails, and never walk loaded objects from a background
  thread without that.
- No per-slot kernel calls in a sweep. Under the anti-cheat one
  `VirtualQuery` can cost a millisecond.
- Anything the game might still hold a pointer to is not yours to free.
- Add an `FX_ASI_BUILD` line to `res/VLights.rc` for each new game build,
  and re-check the byte pattern against Cfx's `LoadStreamingFile.cpp`.

## Diagnosing a report

Ask for `vlights.log` with `debug = 1` and `near_log = 1`, and the
coordinates of the lamp. `docs/diagnostics.md` explains the lines and the
`probe` key; each tier's document ends with the procedure for its kind of
light. For a crash, the offset in FiveM's dialog plus a symbol build
(`docs/diagnostics.md`, "Crash dumps") names the function.
