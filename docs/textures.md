# Lantern textures (the glow on the lamp head)

With every light recoloured, the lantern head still glowed orange. That is
not a light: it is an emissive texture on the lamp's own mesh, multiplied
by a brightness factor by the `emissive` shader, which has no tint
parameter for vanilla streetlights. The only way to change it is to change
the pixels.

## Where the textures are

Mostly **embedded in the model file**, not in a separate dictionary:

| Model | Embedded texture with the glow |
| --- | --- |
| `prop_streetlight_01`, `_05` | `prop_streetlight_head` 256x256 BC1, lens area (148,101,8) |
| `prop_streetlight_01b` | `prop_streetlight_01_bulb` |
| `prop_streetlight_03` family, `prop_wall_light_09a` | `prop_streetlight_03_bulb` 128x128 BC1, (172,101,0)..(238,165,0) |

Separate `.ytd` dictionaries (the `+hi` variants and shared packs) hold
copies with the same names; recolouring only those changed nothing
visible, which is how the embedded copies were found (near-light debug
line `near: yft '...' embedded textures: ...`).

## Reaching the pixels before the GPU does

GPU textures are created when the resource is *placed*, and they are
immutable. So the raw resource is edited before placement:

**Hook.** `strStreamingModule::PlaceResource(store, object, blockMap, name)`
(base slot 6 + the 2802 shift = 12) on the texture, drawable, fragment and
drawable-dictionary stores. FiveM already owns that slot on all of them:
the vtable entry points at a runtime-generated function in heap memory
(not a jump stub; a real body that sets up registers and calls the game's
routine, then tail-jumps into `gta-streaming-five.dll`). The plugin scans
that body for `call rel32`, `call [rip+x]` and `mov reg, imm64` targets and,
if exactly one lies inside the game image, detours that routine. All four
stores resolve to the same routine, so one detour with a per-store
dispatch (by the `store` argument) covers them. Placement runs on a
streaming thread, not the main thread; the detour is still installed from
the main thread inside the first map hook call.

**Block map** (`rage::datResourceMap`, read off a live one; the `name`
argument is empty on this path):

```
+0   u8 virtualCount   +1 u8 physicalCount
+8   void* root        (= chunk 0's dst)
+16  chunks[]: { u64 encodedSrc; void* dst; u64 size }   virtual first
```

Pointers inside the chunk data are still the file's encoding:
`0x5XXXXXXX` = offset in the virtual space, `0x6XXXXXXX` = physical.
Resolve one by finding the chunk whose `encodedSrc <= v < encodedSrc+size`
and adding to its `dst`. Placement is in place: `dst` is where the object
lives afterwards, which is what makes live repaint possible.

**Walk** (all reads bounds-checked against the chunk):

```
pgDictionary<grcTexture>   +0x30 entries ptr, +0x38 u16 count
gtaDrawable                +0x10 grmShaderGroup -> +0x08 embedded dictionary
gtaFragType                +0x30 drawable, +0x38 extra drawables (+0x48 count)
pgDictionary<gtaDrawable>  entries as above, each a drawable
grcTexture (Cfx grcTexture.h)
  +0x28 name ptr   +0x38 ID3D11Resource*   +0x50 u16 width   +0x52 height
  +0x56 stride     +0x58 u32 format        +0x5D u8 mip levels
  +0x70 pixel data ptr (physical)          +0x78 ID3D11ShaderResourceView*
```

Format is a FourCC (`DXT1`, `DXT3`, `DXT5`) in the raw file and a DXGI
number once placed: 71/72 BC1, 74/75 BC2, 77/78 BC3 (second = sRGB),
28/29 R8G8B8A8, 87/91 B8G8R8A8, 21 A8R8G8B8. Mips follow each other in
the data; each level is `ceil(w/4)*ceil(h/4)*blockBytes` for BC formats.

**Pixel pass.** Only textures whose name contains one of `texture_names`
(default `streetlight, wall_light, lamppost, lamp_post, ind_light,
oldlight`). Vehicle indicator textures (`*_lights`, `*_lights_glass`)
share dictionaries with lamps and match the sodium zone, so selection has
to be by name. `texture_exclude` (default `rsn_`) removes false positives
such as `rsn_os_streetlight_orange`, a Rockstar sign artwork that merely
contains the word. `texture_force` (default `prop_streetlight_01_bulb`,
with `all_streetlights` on) retints a texture wholesale, every pixel toward
the target with its brightness kept: that lens is a grey-green tint at
saturation 0.1 to 0.2 throughout, which no zone can single out without
catching half the map. For BC1/2/3 only the two RGB565 endpoints per 4x4 block are
recoloured through the same matcher as the lights. BC1 chooses its block
mode by endpoint order (`c0 > c1`: four colours; otherwise three plus
transparent); if a recolour flips the order the endpoints are swapped back
and the 2-bit indices remapped (`XOR 0x55555555` in four-colour mode; swap
0 and 1 only in three-colour mode), and equal endpoints are nudged apart.
Uncompressed formats are a per-pixel pass.

## Live repaint

Every selected texture is registered at placement with a copy of its
original pixels (all mips, capped at 96 MB total) and the final address of
its texture object. On a colour change, from the worker thread:

1. Recolour the original copy with the current settings.
2. Get the current `ID3D11Resource*` from the object, `QueryInterface` to
   `ID3D11Texture2D`, `GetDesc`, `GetDevice`. Device methods are
   free-threaded, so no immediate context and no render hook.
3. `CreateTexture2D` (immutable, shader-resource, initial data per mip
   with `SysMemPitch` = row pitch) and `CreateShaderResourceView` with the
   old view's description.
4. Swap the view pointer at +0x78 and the resource at +0x38 with
   interlocked exchanges. The renderer picks them up next draw.
5. Keep the previous pair alive. Everything we created for a texture is
   released only when that texture unloads: the game's hi-detail swap
   (`+hi` dictionaries) moves resource pointers between texture objects
   and restores them later, so a pointer we released early can resurface
   in the game's hands (a game-side crash in the streaming code, 0.19.1).
   An unfamiliar pointer in the object means the game is mid-swap; the
   entry is skipped that round. Before every repaint the store's pool is
   checked to confirm the resource is still the one registered.

The plugin remembers a hash of the pixels the game's own GPU texture was
built from and of what is on screen. With the plugin disabled (or a
config that matches nothing) the wanted pixels are the untouched
originals: if the game's texture already holds them its pointers go back,
otherwise a texture is built from the original copy, since the game's one
was made from recoloured data at load. Rebuild requests are coalesced to at most four per
second so slider drags do not create textures every frame. On unload
(`Remove` detour, shared with the light registry through
`track::SetRemoveListener`) the game's pointers are restored before it
frees the object and the entry is dropped. Confirmed: 42 textures rebuilt
per change, none failed.

## If a lantern is the wrong colour

1. `debug = 1`, restart, F9. `textures:` reports resources placed, how
   many held selected textures, textures/blocks recoloured, and how many
   are registered. Each selected texture logs `tex: <store> slot N 'name'
   FMT: K blocks recoloured`. Zero blocks on a lamp texture means its
   pixels did not match the zones.
2. `near: yft 'pack:/prop_x' embedded textures: ...` (debug) lists the
   names inside the model. If the glow texture's name is not covered by
   `texture_names`, add it. The `unselected light-related dictionary`
   lines list `.ytd` candidates.
3. `tex: <store>: PlaceResource slot 12 ... could not be resolved` or `N
   distinct game-image call targets` means FiveM changed its handler; the
   scan needs updating. The hook lines at start say which routine each
   store resolved to.
4. `tex: live rebuild of 'name' failed: <why>` names the Direct3D step.
   "format differs" or "fewer mips kept" means the placed texture is not
   what the raw data described.
