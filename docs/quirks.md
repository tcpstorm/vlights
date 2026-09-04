# Quirks: FiveM, the anti-cheat, and the engine

Everything here cost at least one crash or one wasted evening. Check this
list before adding a hook or a check.

## The six-slot vtable shift (builds 2802 and newer)

Cfx's `Streaming.h` lists `strStreamingModule`'s virtuals in base order
(dtor, FindSlotFromHashKey, FindSlot, Remove, RemoveSlot, Load,
PlaceResource, SetResource, GetPtr, GetDataPtr, Defragment, ...). Its
`XBRVirtual.h` declares the class with `XBR_VIRTUAL_BASE_2802(0)`, which
adds **six** slots at the front on builds >= 2802. On b3751:

| Base | Actual | Virtual |
| --- | --- | --- |
| 3 | 9 | `Remove` (unload; what the live-repaint registry must hear) |
| 6 | 12 | `PlaceResource` (raw resource, before GPU objects exist) |
| 7 | 13 | `SetResource` (publishes a loaded asset; the "load-complete" hook) |
| 8 | 14 | `GetPtr` |
| 2 | 8 | `FindSlot(uint32_t* id, name)`: writes through its first argument |

`hook/pattern.h: StreamingVtableShift()` derives the shift from the build
number in the process name (`FiveM_b3751_GTAProcess.exe`). Every earlier
mystery was this shift: "slot 8 writes through a null array" was
`FindSlot` writing its result through the index we passed as a pointer;
"slot 13 is load-complete" is `SetResource`; and the registry detouring
slot 3 was detouring an unnamed base virtual, so it never saw an unload
and repaints wrote into freed blocks. `grcTexture` has the same shift
(irrelevant here: only its data fields are used).

## What the anti-cheat (adhesive) tolerates

Terminates the game ("Early-exit trap", ~8-60 s after the offence):

- an `IDXGISwapChain::Present` hook (menu became its own window instead);
- writing to a vtable in the game's read-only data (destructor slot
  patch; replaced by code detours + pool checks).

Tolerated so far:

- MinHook `.text` detours inside the game image, installed from the only
  thread that calls the target;
- detours on the game routine that FiveM's own heap-generated handlers
  call (the placement hook);
- creating Direct3D textures on the game's device from a worker thread and
  swapping pointers inside heap objects;
- a separate top-level window with its own D3D11 device for the menu.

Not tried: hooking FiveM's heap-generated code directly, hooking inside
Cfx DLLs, immediate-context calls.

`VirtualQuery` is intercepted: on heap addresses it has measured anywhere
from 4 microseconds to 1.3 milliseconds per call between sessions. A
`ReadProcessMemory` self-probe stays under a microsecond. Both are timed
at the first map load (`near: memory checks:` line) and the probe is
preferred. Startup sweeps must never do per-slot syscalls: 235k pool
slots x milliseconds looked like a hang at `ambient_SD.ipl`.

## MinHook under FiveM

FiveM stubs `CreateToolhelp32Snapshot` (fails with no error code); stock
MinHook reports that as `MH_ERROR_MEMORY_ALLOC` and refuses to hook. The
vendored copy (`third_party/minhook`) is patched: `MH_TOLERATE_FREEZE_FAILURE`,
`mh_vlights_allow_suspend = 0` (never suspend threads; safe because
every detour is installed from the only thread that calls its target, or
from `DllMain` while that thread is blocked in `LoadLibrary`), an
`NtGetNextThread` fallback behind the flag, and an aligned 8-byte store
for the jump so a concurrent reader never sees a half-written patch.

## Streaming stores

- Never call model-store slot 14 (`GetPtr`, or 8 in base terms) blindly on
  a slot; use the pool entry at +0 (`atPoolBase` at store+56, name hash at
  +12). For `.ymap` the entry's +0 is not the `CMapData`; the registry
  keys on the name hash there and records `CMapData::name` itself.
- Hooking `Load` / `SetResource` on the *base* stubs shared by every store
  during session init crashed (`CExtraContentWrapper`); the model stores'
  own 22-byte `SetResource` stub is fine.
- Placement (`PlaceResource`) runs on a streaming thread. Load-complete
  (`SetResource`) and `Remove` run on the main thread.
- A store's pool keeps a slot's name after the object is freed. Liveness
  by name alone is not enough; the `Remove` detour must be on the right
  slot (above).

## Walking game memory

- Never walk loaded objects from a background thread without probing each
  pointer (`ReadProcessMemory` self-probe) first, and never trust a
  distance check on floats that may be NaN (`!(d2 <= r2)`). The F9 walk
  over hundreds of blocks did both wrong once and crashed.
- Load-time hooks may trust the object they are handed (fresh resource,
  owning thread); repaint-time walks may not.
- FiveM redirects some vtables to heap copies (the streaming stores', and
  the ones its handlers replace). "vtable inside the game image" is a
  fine check for placed models and extensions, and a wrong one for store
  objects.

## Loading the plugin

- FiveM refuses an `.asi` that does not declare the running game build:
  `res/VLights.rc` has one `FX_ASI_BUILD <build>` resource per
  supported build (2189..3889). Add one when a build lands.
- `sv_pureLevel >= 2` blocks all plugins; `sv_scriptHookAllowed` is
  irrelevant (no SHV natives used).
- Windows Smart App Control can block a freshly built, unsigned `.asi` on
  first load ("Couldn't load VLights.asi", Code Integrity event
  3077); its verdict flips on the next launch. `build.ps1` checks by
  loading the file through `NativeLibrary::Load`.
- The crash dialog helper `FiveM_DumpServer` keeps the `.asi` open after a
  crash; close it before installing. Installing by rename-then-copy works
  while the game runs (the next launch picks it up).

## The engine copies things

- Light entities copy `CLightAttr` / `CLightAttrDef` at spawn: near lights
  only change as they stream back in (near-lights.md).
- GPU textures are immutable and created at placement: texture changes
  need a rebuild + pointer swap (textures.md).
- LOD light arrays are read in place: they repaint live (lod-lights.md).
