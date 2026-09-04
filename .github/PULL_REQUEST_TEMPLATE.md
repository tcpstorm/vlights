<!-- One topic per pull request. See CONTRIBUTING.md. -->

## What this changes

<!-- What, and why. Link the issue if there is one. -->

## Kind of change

- [ ] Plugin code (`src/`, `res/`, `tests/`, build files): `VERSION` is bumped and the merge will publish a release
- [ ] Docs, media, tooling, workflows only: no version bump

## If a game hook, memory layout, vtable slot, or byte pattern changed

- Game build tested on:
- Startup lines from `vlights.log` (every `... detoured` line):

```
```

- Tested in-game: a session of driving and flying at night with the menu open, colours changed live, no crash

## Checklist

- [ ] Builds with `.\build.ps1` (tests run inside the build)
- [ ] Docs updated in the same PR (ini template in `src/plugin/config.cpp`, README table if a user would care, `docs/` for anything technical)
- [ ] Shipped `vlights.ini` regenerated if config keys changed
- [ ] No attribution trailers or tool advertisements in commits
