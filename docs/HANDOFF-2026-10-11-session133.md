# Handoff — 2026-10-11, session 133: the black head is captured, and the map unit's sprite is a two-texture quad whose data is right at dump time

Developer request: *"read the latest handoff docs, and continue the work on fixing
the black sprites. here's one example of the issue. I can help with drive the game
to a screen where this happens"*, then, after the drive: *"the character's head is
black"*, *"the character units are not 3d models. they are composed of multiple 2d
sprites ... other sprites are used for the trunk, legs"*.

**Result.** The fault is captured — five key presses on `scene_14.bin`, the first
with the head correct and the other four with it black, and the four failing
presses are byte-identical. The tooling to read a capture is new
(`tools/ogre_frame.py`), and it established that **the whole rendered frame is in
the dump**: the game's own framebuffer at `0x80000400` (and its two siblings) is
the frame the VI scans out, so the fault can be looked at offline with no
screenshot. The black head is a **flat dark colour over the correct head
silhouette**, and the silhouette is right while the colour is not.

The unit is 14 quads, each two triangles, each drawn from **two textures**: an I4
mask sampled from an I8 source at TMEM 408 and an RGBA16 colour at TMEM 0. The
head is the 16x16/16x15 square part. In the failing presses it is drawn from a
block at `0x802FB0E0` that `func_8019ABC4` builds at runtime (two `36 34` headers
plus two payloads); in the correct press it comes from the preloaded run at
`0x802C5280`..`0x802C9EB0`.

**The block's bytes are correct when the dump is taken.** Both payloads decode to
the head-down art, the row stride is 16 texels by a row-similarity measurement,
and the draw's state (tiles, TMEM addresses, strides, windows, combiner, prim,
env) is identical in the two presses. The colour the framebuffer holds is the
**ENV colour**: `0x2947` in RGB555 = `(5,5,3)` = `0x2A291F >> 3`, and `0x2A291F`
is the `G_SETENVCOLOR` the same frame sets. So the renderer drew the head with the
texture's alpha and *no* texture RGB.

That leaves one class of cause, and §6 names the single experiment that separates
it: the renderer read the two payloads at a moment when they were not the bytes
the dump holds.

## 1. What a capture is, and the new reader

`debug/frame-capture.sh` is the one-command capture used here: it sets
`OGRE_RAM_KEY=1 OGRE_RAM_KEY_FULL=1 OGRE_RAM_KEY_OUT=<outdir>`,
`OGRE_NO_AUDIO=1`, `OGRE_PREF_DIR=<outdir>/pref` and `OGRE_SCENE_LOG=1`, loads a
save, and tees `run.log`. **Every press writes another whole image**
(`ram-1-full.bin`, `ram-2-full.bin`, …), which is what makes a five-press run
one experiment.

`tools/ogre_frame.py` (new) renders a frame out of such an image:

```
tools/ogre_frame.py <dump> --log <run.log> --out <dir>   # one PNG per DL root the log names
tools/ogre_frame.py <dump> --dl 0x802E2490 --out f.png   # one display list
tools/ogre_frame.py <dump> --dl 0x802E2490 --draws       # the draws, no image
```

It walks `G_SETTIMG`/`G_SETTILE`/`G_SETTILESIZE`/`G_LOADBLOCK`/`G_LOADTILE`/
`G_LOADTLUT` per tile and models the RDP's TMEM indirection (`G_TEXRECT` samples
tile T at TMEM `T.tmem`, and the RDRAM behind it is the image the **last load**
bound, not the `G_SETTIMG` that happens to precede the rectangle), the tile's
`line` as the sampling stride, the palette at TMEM `0x100 + 16*palette`, and
CI4/CI8/RGBA16/RGBA32/IA/I decode. It composites `G_TEXRECT` only: **textured
triangles are not drawn**, which is why the first renders showed the dialogue box
and nothing under it.

That limit is why the framebuffer matters. `D_800A8204 = {0x80000400, 0x80025C00,
0x8004B400}`, `0x25800` apart, 320x240 RGBA5551: the three hold the same rendered
frame (they differ in 2-9 bytes of 153600 sampled), including the 3D terrain and
the units that the TEXRECT renderer cannot draw. Rendering one is three lines of
Python and needs no screenshot and no `OGRE_CAPTURE_PRESENT` (which yields black
PPMs on this machine).

## 2. The five presses

`debug/frame-capture.sh` with the default `saves/scene_14.bin`; the developer
pressed `1` five times: the first while the head was up, the other four while it
was down.

| press | head in `0x80000400` | `0x802FB0E0` in the dump |
|---|---|---|
| 1 | a purple/lavender head | `00 00 c0 c1 c1 81 01 40 …` — no header, pixel data |
| 2-5 | a flat `0x2947` head-shaped block | `36 34 00 02 00 0f 00 0f` + 480-byte payload + `36 34 04 00 00 0f 00 0f` at `+0x1E8` + 120-byte payload |

Presses 2-5 are **byte-identical** at that block (payload 1 sha256 `e82c570d7aa50f5e`,
payload 2 `39daa7735ed14fa3`), so the content is deterministic, not uninitialised
heap. Press 1 differs in both payloads (sha256 `e0811a3edc29f558` / `da1f5e81023fc935`).

## 3. The unit is 14 two-texture quads

A command census of the frame's display list at `0x802E2490`:

```
SETTIMG 208  LOADTILE 142  VTX 91  TRI2 91  LOADBLOCK 59  TEXRECT 59  MTX 58
```

The 45 leading `TRI2`s are the background, each preceded by a `LOADTILE` from
`0x80243E18 + n*0xF00` (RGBA16, w=320) — the njpeg-assembled image the
`docs/guides/njpeg-backgrounds.md` pipeline produces. Then 14 parts, each:

```
MTX (identity)  VTX 4 (one quad)  SETTIMG I8  SETTILE tile7 I8 line=1 tmem=408
LOADTILE → SETTILE tile0 I4 tmem=408  SETTILESIZE  SETTIMG RGBA16
SETTILE tile7 RGBA16 line=4 tmem=0  LOADTILE → SETTILE tile1 RGBA16 tmem=0
SETTILESIZE  TRI2 (0,3,1)(0,2,3)
```

so TEXEL0 is the I4 mask and TEXEL1 the RGBA16 colour, both loaded per part. The
14 parts are `12x19, 20x30, 20x25, 12x20, 8x6, 32x24, 16x15, 28x47, 16x23,
12x22, 24x28, 32x29, 24x29, 12x16`; the list is emitted twice in the frame (two
passes at `0x802E31xx` and `0x802E3Bxx`).

The head is the **16x16 / 16x15 square part**, identified two ways:

* it is the only square ~16x16 part, and the head on screen is ~18x14 px;
* the correct press's colour texture at `0x802C6BE8` renders as a **purple head**,
  which is the colour the correct head has on screen.

| | correct press | failing presses |
|---|---|---|
| colour (RGBA16) | `0x802C6BE8` (block `0x802C6BE0`, 16x16) | `0x802FB0E8` (block `0x802FB0E0`, 15x15) |
| mask (I8) | `0x802C6DF0` (block `0x802C6DE8`) | `0x802FB2D0` (block `0x802FB2C8`) |
| provenance | inside the preloaded run `0x802C5280`..`0x802C9EB0` | a runtime `malloc` by `func_8019ABC4` |

A walk of every texture load in each dump (seeds = every `G_SETTIMG`, walk forward)
gives the same answer: `0x802FB0E8` and `0x802FB2D0` are loaded **only** in the
failing presses, and `0x802C6BE8`, `0x802C6DF0`, `0x802C6F58`, `0x802C7A60` only in
the correct one.

## 4. The two payloads are correct at dump time

The block is `[header 0x0002 15x15][payload 480 B][header 0x0400 15x15][payload
120 B]` (the second header sits at `+8+480 = 0x802FB2C8`, which is the size
`func_8019ABC4` computes: `s7 = ((w+3)>>2)<<3)*h` = 480 for a 15-wide RGBA16, and
`fp` = 120 for the I8).

Decoded with the strides the loader uses (16 texels/row for the RGBA16, 8 bytes
for the I8), the failing payload is a **plausible head-down image**: a purple head
with the same palette as the correct one, and a mask that is a coherent white
blob. The row stride is 16 texels, not 15: the mean absolute row-to-row difference
over strides 14/15/16/17 is `13387 / 12438 / 6259 / 10477` for the failing payload
and `10015 / 7547 / 4785 / 6380` for the correct one, so the storage is 16-wide in
both and there is no shear.

## 5. The draw's state is identical; the colour that reaches the screen is ENV

The head's draw block is the same command for command in the two presses (all
values below are identical; only the source addresses and the row counts differ,
`lrt=60` = 16 rows in the correct press against `lrt=56` = 15 in the failing one):

```
SETTILE tile7 fmt=4 siz=1 line=1 tmem=408   LOADTILE lrs=30 lrt=5x
SETTILE tile0 fmt=4 siz=0 line=1 tmem=408   SETTILESIZE 0,0..60,5x
SETTILE tile7 fmt=0 siz=2 line=4 tmem=0     LOADTILE lrs=60 lrt=5x
SETTILE tile1 fmt=0 siz=2 line=4 tmem=0     SETTILESIZE 0,0..60,5x
PRIMCOLOR FFFFFF40  TRI2 (0,3,1)(0,2,3)
```

`G_SETCOMBINE` is `FC62FE03 2F0D73FD` in both.

The pixels the framebuffer holds over the head are:

```
y 88  3189 2947 2947 2947 2947 2947 41c9 4a4b …
y 90  2947 2947 2947 2947 2947 4a09 39c7 528d …
```

`0x2947` = RGB555 `(5,5,3)` = `(41,41,25)` in 8-bit, and the frame's own
`G_SETENVCOLOR` is `0x2A291F` = `(42,41,31)`, i.e. `(0x2A>>3, 0x29>>3, 0x1F>>3)`
= `(5,5,3)`. **The head is drawn in the ENV colour, over the texture's alpha
silhouette.** The developer's own capture of the same fault measures the same
thing: the head region of their image is dark olive at luminance 30-50/255, not
black.

## 6. What that leaves, and the one experiment that separates it

The texture bytes are right in the dump, the state is right, and the drawn colour
is a constant the renderer supplies. The remaining hypotheses are:

* **H1 (renderer reads late).** RT64 replays the deferred TMEM load — and so reads
  `0x802FB0E8`/`0x802FB2D0` — on its render thread, after `send_dl` has returned
  and after the game has begun the next frame. `send_dl` calls
  `app_->processDisplayLists(...)` synchronously (`app/src/renderer.cpp:1727`) but
  that builds the workload; the `loadBlockOperation`/`loadTileOperation` copies run
  when the workload is executed. If the game rebuilds or clears the block for the
  next frame in that window, the renderer samples bytes the dump no longer holds.
* **H2 (the game fills after the draw).** The block is built after the display list
  that references it, so the frame always draws the previous content. The same
  observation would follow.

Both are separated by one datum: **what the payload holds at display-list
submission**. `OGRE_TEX_TRACE_IMAGES=1` prints a fingerprint of every image the
list binds, at submission:

```
OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_KEY=2 OGRE_TEX_TRACE_IMAGES=1 debug/frame-capture.sh
```

Press `2` at the black head to arm the trace and `1` to take the dump. If
`[tex] img:` for `0x802FB0E8` reports `nonZero 0` (or a mean far from the dump's)
while the dump holds the head, the renderer is reading a buffer the game has
already replaced, and the fix is a synchronisation one (the port already has the
`ogre_sync_framebuffers` shape for the framebuffer case). If the fingerprint
matches the dump, H1 and H2 are both dead and the fault is inside RT64's sampling
of that tile.

## 7. What was run for verification

| run | evidence |
|---|---|
| `debug/frame-capture.sh` (five presses, `scene_14.bin`) | `ram-1-full.bin`..`ram-5-full.bin`, one whole 8 MiB image per press, exit 0 |
| `tools/ogre_frame.py <dump> --dl 0x802E2490` on the two earlier dumps | the OTA dialogue frame ("OTA / …Do you think there's a way to avoid battle with the…") and the "Western Division" frame, so the walker is right on this screen |
| the same, `--log run.log` | one PNG per root the log names (`[renderer] display list N … data=0x…`); the log's root for the failing press is `0x802E2490` |
| RGBA5551 render of `0x80000400`/`0x80025C00`/`0x8004B400`, press 3 | the mission field, the dialogue box, the two units, and the flat `0x2947` head; the three buffers differ in 2-9 of 153600 sampled bytes |
| the same crop of press 1 | the same unit with a purple head |
| command census of `0x802E2490` | `SETTIMG 208 LOADTILE 142 VTX 91 TRI2 91 LOADBLOCK 59 TEXRECT 59 MTX 58` |
| ordered command listing, unit section | 14 `LOADTILE I8` + 14 `LOADTILE RGBA16` + 14 `TRI2`, each pair with its own `SETTILE`/`SETTILESIZE` |
| `loads.py` over every `G_SETTIMG` in ram-1 and ram-3 | `0x802FB0E8`/`0x802FB2D0` only in the failing press; `0x802C6BE8`/`0x802C6DF0`/`0x802C6F58`/`0x802C7A60` only in the correct one; 6 of 201 loads in the failing frame read all-zero sources (`0x802C59D0`, `0x802C9EB0`, `0x802C9D28`) |
| decode of both head payloads with the loader's strides | a purple head in each; the failing mask is a coherent blob |
| row-to-row similarity over strides 14..17 | 16 is the minimum in both payloads |
| `0x3634`/`0x5554` header scan over both dumps | 37 headers in the correct press, 40 in the failing one; the failing press alone has `0x802FB0E0` and `0x802FB2C8` (15x15) and the source blobs `0x5554` at `0x803FE8D0`/`0x803FEA70` |
| framebuffer pixel read, head box, all three buffers | flat `0x2947`; 0 pure-black pixels in a 28x20 box |
| instruction read of `func_8019ABC4` (`0x8019ABC4`..`0x8019AF0C`) | `s3 = func_8019A1B4(src)`; sizes from `s3[4]`/`s3[6]`; `malloc(s7+fp+0x10)`; writes `36 34 00 02 w h` at the block and `36 34 04 00 w h` at `+8+s7`; then copies (`& 4` set: `fp` bytes into the second payload and a palette expansion into the first; clear: `s7` and `fp` bytes) |

No generated code and no probe were touched: `grep -rl probe RecompiledFuncs/
Bank*Funcs/ app/` is empty.

## 8. Files changed

| file | what |
|---|---|
| `tools/ogre_frame.py` | new: render a frame out of a whole-RDRAM dump (TEXRECT layer) |
| `debug/frame-capture.sh` | new: one press = one whole image, with the log the reader needs |
| `debug/black-sprite-133/` | the scratch readers this session used (`loads.py`, `s2dex.py`, `parts.py`, `montage2.py`, `crop.py`, `fb.py`, `rend2.py`, …) and the rendered evidence (`frames/press*.png`, `crop1.png`, `crop3.png`, `p7b-*.png`, `alpha-*.png`) |
| `docs/notes-black-sprite.md` | the session-133 section |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md` | the record |
| `docs/HANDOFF-2026-10-11-session133.md` | this file |

## 9. Corrections to earlier sessions

* **The unit under the dialogue box is drawn with textured triangles, not
  `G_TEXRECT`s.** A `G_TEXRECT`-only instrument cannot see it, which is the whole
  reason the three earlier captures printed `0 'BLACK DRAW'` lines: the trace was
  watching the wrong primitive, not the wrong window. `docs/guides/
  sprite-mechanisms.md` §1 and §5 present the TEXRECT path as *the* black-part
  path; that is true for the dialogue text and false for the unit.
* **Session 132's `imgw`-based colour decode is wrong for every `G_TEXRECT` whose
  stride comes from the tile.** The stride is the tile's `line` (in 64-bit words)
  and the image is the one the last load into the tile's TMEM bound; the
  `G_SETTIMG` that precedes the rectangle can be the palette load. In this frame
  the trace measured the palette (`0x802FD700`) as the portrait's image with a
  2-byte row stride, which is why a black part could never be reported.
* **Session 132's "the cutscene close-up character" is the map's unit sprite.**
  The screen is the mission field with a dialogue box over it; the unit is the
  map module's multi-part sprite, and its parts are built by `func_8019ABC4`
  (`0x8019ABC4`, the same RAM as unit M, scene `0x05`'s module), called from
  `func_801AD558` (`0x801AD558`, in turn from `func_801AD6C4`).
* **The two earlier pose dumps were not a good/bad pair.** Press 1 here shows the
  correct head *and* a different asset layout at `0x802FB0E0` (no header at all),
  so the block is created when the pose changes; comparing two presses taken
  either side of the change compares two different assets, not two states of one.

## 10. The second driven run: the renderer had the right bytes

The developer drove to the black head again and pressed `1` twice and `2` once
with `OGRE_TEX_TRACE_IMAGES=1` (the script now sets it). Two new whole images
(`ram-1-full.bin`, `ram-2-full.bin`, 18:10) and the trace armed at `t=25016ms`.

The trace's fingerprint is taken **at display-list submission**, and for the
head's two textures at `dl=2029` it reports:

```
[tex] dl=2029 SETTIMG img=0x002FB2D0 raw=0x802FB2D0 fmt=4 siz=1 width=8
[tex]   img: 256 bytes, nonZero 179/256, mean 125.0
[tex] dl=2029 SETTIMG img=0x002FB0E8 raw=0x802FB0E8 fmt=0 siz=2 width=16
[tex]   img: 1024 bytes, nonZero 710/1024, mean 91.1
```

Both payloads are **populated when the list is submitted** (the fingerprint
samples 32 rows, so its `1024 bytes` runs past the 480-byte payload into the
second header and the mask, which is why the non-zero ratio is not 100%). The
new presses also reproduce the fault (head region of `0x80000400`: 71 of 200
sampled pixels are the flat `0x2947`) and are byte-identical to the 17:39 ones at
the asset block.

**RSP-style walk of the frame** (`G_DL` followed with a visited set, state
tracked through the whole list; root `0x802B3260`, the address the log names)
now gives the state at each of the 91 triangles. The unit is drawn **twice**, and
the two passes differ:

| pass | triangles | combiner L/H | prim | env | geometry mode |
|---|---|---|---|---|---|
| 1 | `0x802B4060`..`0x802B4880` | `FCFF97FF` | `FFFFFF40` | `00000000` | `00000004` |
| 2 | `0x802B4A10`..`0x802B5250` | `FC62FE03` / `2F0D73FD` | `FFFFFFFF` | `2A291F01`, `25251D01`, `1E1F1A01` | `00000404` |

Decoded with RT64's own field layout (`shared/rt64_color_combiner.h`:
`B = H>>28`, `C = L>>15`, `D = H>>15` for cycle 0), pass 2 is

```
colour = (1 - TEXEL1) * ENV + TEXEL1        alpha = PRIM_A = 0xFF (opaque)
```

so pass 2 replaces pass 1, and a sampled `TEXEL1` of zero renders **exactly the
ENV colour**. The head's own ENV in that pass is `0x2A291F01`, and `0x2A291F >> 3`
in RGB555 is `0x2947` — the flat colour the framebuffer holds. The head is the
only part whose pixels are flat: its box has 73 distinct values (71 of them
`0x2947`) against 176 and 145 for the trunk and legs of the same unit.

**So the remaining fault is that this draw's `TEXEL1` samples as zero inside
RT64**, with correct RDRAM, correct tile descriptors (the head's `tile0 = I4,
line 1, tmem 408` and `tile1 = RGBA16, line 4, tmem 0` match every other part's
shape) and a u-span/window ratio of 1.87 against 1.73-2.00 for the parts that
render.

**What the fingerprint does and does not settle.** It is taken in `send_dl`,
which calls `processDisplayLists` synchronously but only *builds* the workload:
RT64 reads the texture's RDRAM on the render thread, in
`loadTileOperation`/`loadBlockOperation`'s non-deferred branch, and the shader's
`implLoadTMEM` is what replays the load. So the fingerprint rules out "the buffer
was empty when the list was submitted" and nothing more. If the game rewrites
`0x802FB0E8` for the next frame inside that window, the dump and the fingerprint
would both show the art while the renderer samples a buffer that was zeroed in
between. That is H1 of §6, and it is still open.

**The pattern fits H1.** The one part that fails is the only part whose asset is
built at runtime (the pose changed) — every other part comes from the preloaded
run `0x802C5280`..`0x802C9EB0` and is never rewritten. A part that is rebuilt
every frame loses its texture on every frame; a preloaded one never does.

## 11. The experiment for H1: `OGRE_SYNC_DL`

`app/src/renderer.cpp`'s `send_dl` now has, as an experiment,

```cpp
if (getenv("OGRE_SYNC_DL") != nullptr) {
    sync_framebuffers();     // retires the workload before the submission returns
}
```

`syncFramebuffers` (`rt64_state.cpp:657`) waits on `workloadQueue->waitForWorkloadId`,
so with the knob on the game's CPU cannot write the next frame before the
renderer has read every texture this list binds. The run is slow (the frame rate
is bounded by the renderer), which is why this is an experiment and not a fix.

```sh
OGRE_SYNC_DL=1 debug/frame-capture.sh build-app/ogrebattle64
```

Drive to the black head and look at it: **if the head renders correctly with the
knob on, H1 is confirmed** and the fix belongs where the texture bytes are handed
to RT64 (copy them, or retire only the texture reads, instead of waiting for the
whole workload). If the head is still the flat ENV colour, the fault is inside
RT64's sampling of this draw and the next instrument is a per-draw trace of the
tile index it selects for TEXEL0/TEXEL1 and those tiles'
`fmt/siz/line/tmem/palette`.

Verified so far: the knob builds (`cmake --build build-app`) and a
`OGRE_SCENE=title OGRE_EXIT_AFTER_MS=5000 OGRE_SYNC_DL=1` run exits 0 with no
`error.log`; the head case needs the developer.

## 12. The third run: `OGRE_SYNC_DL` stalls the game, and the tile trace

The developer ran §11's experiment and the game stalled in the cutscene. The log
says it was not a deadlock: display lists kept arriving to `t=45403ms`
(`dl 2330`, about 30 lists/s) and the run ended when the window was closed, so
`sync_framebuffers()` on every submission starved the game's own progress instead
of answering the question. **The knob is withdrawn** — it was removed from
`send_dl` again, so `app/src/renderer.cpp` carries none of it.

That leaves the renderer-side question: the head's `TEXEL1` samples as zero while
its RDRAM, its tile descriptors and (per §10) the bytes at submission are all
right. RT64 decides that per tile in two places, and both are now traced by one
knob, `OGRE_TILE_TRACE_ALL=1` (plus `OGRE_TILE_TRACE_MAX=<n>` for the line cap):

* `State::fullSyncFramebufferPairTiles`' `uploadTile` either **reads RDRAM**
  (`uploadTexture` / `uploadTMEM`) or takes `uploadEmpty`, which reads nothing and
  assumes the texture is already in the cache;
* `FramebufferRenderer::createGPUTiles` asks the cache for the hash and, when the
  hash is missing, hands the draw the **fallback** texture.

The widest line is the one the port already had (`OGRE_TILE_TRACE`), extended
with the tile's `fmt/siz/tmem/line` and whether the cache lookup succeeded:

```
[gpu-tile] n=2 hash=C775E09AEE957C71 tmemHash=C775E09A957C71 idx=1 dims=20x34 tcScale=1.0000,1.0000
  sample=20x34 rawTMEM=0 copy=0 valid=1 replaced=0 fmt=0 siz=2 tmem=0 line=5 found=1
```

**A correction this made.** The existing comment read "textureIndex 0 means the
hash was not in the cache and the draw samples the fallback". A title-scene run
gives `found=1` for **2500 of 2500** tiles, including every `idx=0` one, so
`idx=0` is a real cached texture and `found` is the field that means what the
comment said. The comment is corrected, and the `found` flag is what the next run
is read for.

Verified: `build-app` rebuilt with the trace; `OGRE_MOD_TEST=1
OGRE_PREF_DIR=/tmp/pref-test OGRE_TILE_TRACE_ALL=1 OGRE_TILE_TRACE_MAX=2500` on the
boot reaches the title and writes 2500 `[gpu-tile]` lines, all `found=1`, with
`idx=0` on the I4 mask tiles (`fmt=4 siz=0`, `tmem=365`) and `idx=1..5` on the
RGBA16 colour tiles (`fmt=0 siz=2`, `tmem=0`). `patches/rt64-ob64.patch` was
regenerated from `tools/RT64` (2103 lines; the `src/contrib/plume` submodule
pointer hunk is stripped, as the tracked patch has always done).

**The run to ask for** (no key press needed; the trace prints by itself):

```sh
OGRE_TILE_TRACE_ALL=1 OGRE_TILE_TRACE_MAX=20000 debug/frame-capture.sh build-app/ogrebattle64
```

Drive to the black head, then close the window. The head's colour tile is the
`fmt=0 siz=2 sample=15x14 tmem=0` line and its mask the `fmt=4 siz=0 sample=16x15
tmem=408` line; a part that renders correctly in the same frame gives the control.

## 13. The fourth run, and the instrument that answers it in one line

The developer ran §12's trace to the black head. The log holds exactly 20000
`[gpu-tile]` lines and **not one of them is a cache miss**: `found=0` appears zero
times, and the cap was consumed before the unit's tiles were reached — the scene's
full-width njpeg strips (`sample=320x6`, 18370 of the 20000) and other units'
`32x32`/`64x64` textures fill it. So that run does not answer the question, and
the cap is the reason.

The instrument is now self-limiting instead: **`OGRE_TILE_TRACE_MISS=1` prints
only the tiles whose texture-cache lookup failed**, i.e. the tiles that sample
RT64's fallback texture. With the knob on, the boot and title scene produce an
empty `[gpu-tile]` list (verified: 0 lines over a 20 s run), which matches §12's
`found=1` for 2500 of 2500 tiles.

The run to ask for, and the log is small enough to read in full:

```sh
OGRE_TILE_TRACE_ALL=1 OGRE_TILE_TRACE_MISS=1 OGRE_TILE_TRACE_MAX=2000 \
  debug/frame-capture.sh build-app/ogrebattle64
```

Drive to the black head, close the window, then

```sh
grep "\[gpu-tile\]" ~/Desktop/ogre-battle-64-recomp/frame-capture/run.log
```

* **A line naming a tile** (its `fmt/siz/tmem/line`, `sample`, `hash`, and
  `tmemHash`) is the fault: the draw samples the fallback because the tile's
  texture was never uploaded, and `State::fullSyncFramebufferPairTiles`'s
  `uploadTile` is where that tile's upload path goes wrong (`uploadEmpty` assumes
  the texture is already in the cache).
* **No line at all** says no draw in the run samples the fallback, so the fault is
  inside the sampler for a tile that *is* in the cache: the next instrument is the
  shader-side path (`sampleTexture` → `TextureDecoder.hlsli`), where the head's
  I4-from-I8 mask and its two-tile pair are the only parts of the unit that differ
  from a part that renders.

Also in this round: `OGRE_TILE_TRACE_MAXH=<n>` restricts the trace to tiles whose
sample is at most `n` by `n` (the wide layers otherwise consume the cap), and
`patches/rt64-ob64.patch` was regenerated (2118 lines).

**State of the fix.** None of the four hypotheses has been confirmed yet: the
RDRAM is right (§10), the tile descriptors match every part that renders (§10),
and the cache does not miss on the title scene (§13). The remaining candidates are
the fallback-sample case above and the shader's sampling of this tile, and the
single run above separates them.

## 14. The fifth run: the texture cache never misses, so the sample itself is zero

The developer ran §13's command to the black head. `grep "\[gpu-tile\]" run.log`
returns **nothing**: no tile in the run samples RT64's fallback texture. That
closes the upload/cache branch, and it matches §12's `found=1` for 2500 of 2500
tiles on the title.

Everything the *display list* says is now verified from the failing dump
(`ram-1-full.bin`, the second press of the 18:10 pair), with an RSP-style walk
(`G_DL` followed with a visited set) that tracks the render state through the
whole list:

| what the sampler reads | value at the head's draws |
|---|---|
| `G_TEXTURE` | `tile=0, level=0, on=1` — the same for all 14 parts |
| `othermode` text LOD (H) | `G_TL_TILE` (`textLOD=0`), so RT64's `computeLOD` does **not** run and `tileIndex0/tileIndex1` stay `0/1` |
| `TEXEL0` / `TEXEL1` | tile 0 = the I4 mask at `tmem 408`, tile 1 = the RGBA16 colour at `tmem 0` |
| cycle type | 2-cycle (`OML=00504240`/`0C184240`) |
| combiner L/H | `FC62FE03` / `2F0D73FD`; cycle 0 = `(1-TEXEL1)*ENV + TEXEL1`, cycle 1 = `COMBINED * PRIM` with `PRIM = 0xFFFFFFFF`, so the final colour is cycle 0 |
| ENV at the head | `0x2A291F01`, and `0x2A291F >> 3` is `0x2947`, the flat colour on screen |
| `SETCIMG` in the frame | one, `0x00025C00`, spanning `0x25C00..0x4B400` — no framebuffer overlaps the head's texture at `0x802FB0E8` |

So `TEXEL1` must be zero *at the sample*: the RDRAM is right (§10), the tile
descriptors match every part that renders (§10), the hash is in the cache (§14),
the LOD path does not aliase the tiles (§14), and the draw is not a tile copy.

**Next instrument: dump RT64's TMEM after the load replay.** The sampling itself
is in the shader (`src/shaders/TextureSampler.hlsli` and
`TextureDecoder.hlsli`; `implLoadTMEM` computes the texel's TMEM address from the
load's `textureStart`/`rowSize`), so the CPU-side mirror of it is to read the
`RDP::TMEM` array (`rt64_rdp.h`) once the deferred loads have replayed for the
frame that draws the head, and compare the bytes at `tmem 0` (the colour tile)
with the RDRAM source at `0x802FB0E8`. A `OGRE_TMEM_DUMP=<path>` written from
`State::fullSyncFramebufferPairTiles` (where the replay happens) at the first
draw call whose tile's load address is `0x802FB0E8` gives exactly that. The two
things that would show up: the load wrote somewhere other than where the sampler
reads, or the load wrote zeros because its source range was miscomputed for a
15-row tile.

**State of the fix.** No layer has been confirmed faulty yet: the RDRAM bytes, the
tile descriptors, the cache lookup, the LOD path, the cycle type, the combiner,
the ENV and the framebuffer-overlap test are all ruled out for this draw. The
remaining suspect is the deferred TMEM load or the shader's texel address for this
one tile, and the TMEM dump above is the discriminating experiment.
