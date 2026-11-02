# Widescreen: making a screen fill 16:9

How RT64's Expand treats 3D and 2D draws, the extended-GBI rules the patched
screens rely on, how to patch a screen's draw, and the patched screens as worked
examples. The WIDESCREEN setting and its window behaviour are in `app-build.md`
→ "WIDESCREEN".

## 1. What Expand does

`app/src/widescreen.cpp` turns RT64's `AspectRatio::Expand` on while the
dispatcher's scene is in `expand_scenes()`: `0x03`, `0x0D`, `0x05`, `0x06`, plus
every scene in the HD-background pack (`hd-backgrounds.md`). `OGRE_WS_SCENES`
replaces the default set for a run. Expand makes the render target wider by
`aspectRatioScale = target / source` (`rt64_workload_queue.cpp`,
`threadConfigurationUpdate`), where `target` is the swap chain's aspect and
`source` the VI's 4:3. Each side gains `E = 160 * (scale - 1)` pixels in
320-wide units; at 16:9, `E = 53.3`.

- **3D projections.** `ProjectionProcessor::processScene` divides a projection's
  x by `scale` when its scissor and viewport cover the whole width and are wider
  than tall. The scene shows more at the sides. The mission (`0x03`) and the
  dialogue backdrop (`0x0D`) widen this way with no game patch.
- **2D rectangles** (`G_TEXRECT`, `G_FILLRECT`). `convertFixedRect`
  (`rt64_framebuffer_renderer.cpp`) keeps the rectangle's native size and shifts
  it by `E`, so the 4:3 picture stays centred and the sides stay black.
- **2D drawn with triangles** under an orthographic projection laid out for
  320x240. Expand counter-scales the projection, so every object keeps its native
  size and position, centred. The sides show only what the game drew past x = 0
  and x = 320. The Organize Screen is this case:
  `guOrtho(-160, 160, -120, 120, -10, 100, 1)` and a background quad of exactly
  320x240.

To classify a screen, run it with `OGRE_DL_ANALYZE=1`; the `[dl-analyze]` line
counts `tri1/tri2` against `texrect`. Triangles alone do not make a screen 3D:
read the projection (`G_MTX` with the projection flag) from an RDRAM dump and
project a few vertices. The quick check is to add the scene with
`OGRE_WS_SCENES=<ids>` and capture a present: black sides around an unchanged
picture mean the screen needs a patch. Ask the developer what the extra width
should show before building (AGENTS rule 1).

## 2. RT64 extended GBI, as this port uses it

Header: `tools/RT64/include/rt64_extended_gbi.h`. Handlers:
`tools/RT64/src/gbi/rt64_gbi_extended.cpp`. Origin maths: `RDP::movedFromOrigin`
(`rt64_rdp.cpp`), `convertFixedRect` and `convertViewportRect`.

- **Enable.** `gEXEnable` is RT64's hook on `G_SPNOOP`; for F3DEX2 the words are
  `E0525464 10000064`. Emit it before the first `0x64` command of the list. Check
  the screen's ucode first (AGENTS rule 8).
- **`extAspectRatio` must be `Expand`.** With RT64's default `Original`, LEFT and
  RIGHT origins collapse to the picture's centre. `set_application_user_config`
  (`app/src/renderer.cpp`) sets it from `ar_option`.
- **Origins.** A LEFT-origin x is the native x measured from the wide picture's
  left edge. A RIGHT-origin x is written as `x - 320` (in 10.2, `x*4 - 1280`),
  because `movedFromOrigin` adds the colour image's width, and is measured from
  the right edge. A NONE-origin x stays centred, like a plain `G_TEXRECT`.
- **Mixed origins stretch.** A rectangle with a LEFT left edge and a RIGHT right
  edge takes its UVs from its native width (`RDP::drawRect`), so its texture
  stretches across the extra `2E`. Use it only on art that is uniform along x.
- **Alignment state.** `gEXSetRectAlign` applies to later plain `G_TEXRECT`s, not
  to `gEXTextureRectangle`, which carries its own origins. `gEXSetScissorAlign`
  applies to later plain `G_SETSCISSOR`s; set it when the game issues its own
  full-screen scissor after draws moved to the edges. Reset both when the
  screen's draw ends, because the state persists into menus drawn after it.
- **Signed coordinates.** `gEXTextureRectangle` takes signed 16-bit 10.2
  coordinates, so it can draw left of native x = 0; a plain `G_TEXRECT` cannot.
- **`gEXFillRectangle`** (`G_EX_FILLRECT_V1`; this RT64 has no macro for it) is
  dropped when the raw `lrx < ulx`, before origins apply. For a LEFT..RIGHT fill,
  write `ulx = 0` and the RIGHT-origin `lrx = 0`.
- **Viewports anchor triangles.** `gEXSetViewportAlign` applies when a viewport
  is next loaded, so set it and then re-issue the game's own viewport (`G_MOVEMEM`
  `DC080008 <address>`). RT64 does not counter-scale a projection whose viewport
  has an origin, so triangles under it keep their native layout measured from
  that edge. Put the alignment back to NONE and re-issue the viewport afterwards.
- **Keep full-screen scissors at the game's extent, (0,0)-(319,239).** RT64
  counter-scales a projection only when its scissor and viewport cover the frame
  pair's merged scissor (`coversWholeWidth`), and rectangles' scissors merge into
  it too (`RDP::drawRect`). An added scissor that reaches x = 320 widens the
  merged scissor, the game's full-screen projections stop qualifying, and the
  sides show stale pixels or the picture falls back to 4:3. `Writer::scissor_full()`
  is (0,0)-(319,239) with LEFT/RIGHT origins.
- **Full-width rectangles stretch.** A NONE-origin rectangle that spans the
  frame pair's scissor width is drawn stretched to the wide picture
  (`coversScissorWidth`), and so are tile copies.
  `gEXSetRectAspect(G_EX_ASPECT_ADJUST)` (`0x64000033 00000002`) keeps it at its
  native size; set the aspect back to AUTO after.
- **Stretched rectangles need `gEXForceUpscale2D`.** The app sets RT64's
  Upscale2D to Original, which snaps a rectangle's UVs to the native pixel; a
  rectangle stretched by a non-integer factor then doubles rows. Force the
  upscale around stretched draws and reset it.
- **Copy mode** divides `dsdx` by 4 in `drawTexRect`. Copy the game's own
  `dsdx`/`dtdy` and cycle type when adding rectangles next to the game's.
- **Native-space checks.** RT64 intersects the scissor and the rectangle in
  native coordinates. An extension draw with no native overlap with an
  edge-anchored scissor makes RT64 track an unbounded texcoord range, which is
  harmless.

## 3. Patching a screen's draw

The bank's generated C is patched with `[[patches.hook]]` entries in
`config/banks/config-bank<U>.toml` (`config/config.toml` for `RecompiledFuncs`).
Each hook is C text inserted before one instruction that calls an `extern "C"`
function in the app. The shared pieces are in `app/src/widescreen_gbi.hpp`:
guest-memory access, a `Writer` that appends commands, the extended-GBI
encoders, and `widescreen_2d_extension()`. `widescreen.cpp` publishes `E` there
whenever Expand is on and 0 otherwise; every hook is a no-op at 0, and each runs
only inside its own scene's code. A new screen needs its id in
`expand_scenes()`.

- **The display list.** Every emitter appends at gDisplayListHead,
  `*(u32*)0x800E9BA0`. Read it, write 8-byte commands, store the new head.
- **Cached bases.** Inside a loop the game often keeps the block base in a
  register and writes the next block as `sw ..., 96($t3)`. A hook that appends
  bytes there must also add them to that register (`ctx->r11 += n` for `$t3`).
- **Rewriting a loop's output.** After the loop exits, the blocks are contiguous
  and of fixed size. Copy them out, validate each block's opcodes, write the
  expanded blocks from the loop's start, then move the head and the cached
  register. Leave the list untouched when a signature does not match, and log
  it once.
- **Hook placement.** A hook goes before the instruction and after its label, so
  branch targets are fine. Do not hook a delay slot. Do not `return` from a hook:
  the function's own exit calls `recomp_trace_return`, and skipping it leaves a
  frame on the runtime's call stack. Hook before the function's `jr ra` instead.
- **Registers.** `ctx->rN` is the MIPS register number: `$v0` 2, `$a0`..`$a3`
  4..7, `$t0`..`$t7` 8..15, `$s0`..`$s7` 16..23, `$t8` 24, `$s8` 30. Values are
  sign-extended 64-bit.
- **Game logic.** A wider view usually needs scroll limits, cursor bounds and
  the mouse's window-to-framebuffer mapping changed too. The bounds are often
  immediates in `slti`/`addiu`; a hook after the compare rewrites the register.
- **The extension value.** `E` comes from RT64's swap chain size, published at
  each present (`widescreen_note_swapchain`), because SDL's window size is stale
  in fullscreen. It is rounded up so limits keep the view on the art; draw
  extensions a little wider than `E` and let the scissor trim them.
- **List capacity.** Measure the screen's list (the last address in an
  `OGRE_DL_DECODE` dump) against its buffer. The map's list uses 13.5 KB of a
  64 KB buffer (`0x8027C140` and `0x8028C120`); the wide patch adds about 9 KB.

### Regenerating one bank

In the WSL clone (`app-build.md` → "Windows setup"), for bank M: copy the edited
config into the clone with CRs stripped, `rm -rf BankMFuncs`, run
`tools/N64Recomp/build/N64Recomp config/banks/config-bankM.toml`, run the unit's
post-generation script (`python3 tools/map_cursor.py` for M,
`tools/mission_cursor.py` for N; the `bank-recomp` target lists them), and copy
`BankMFuncs/` back. Diff the new `funcs_0.c` against the old one: the hooks
should be the only change. Copy back only the bank you changed.

## 4. The world map (`0x05`)

Code: `app/src/map_widescreen.cpp` (hook bodies), `config/banks/config-bankM.toml`
(hook sites), `app/src/map_cursor.cpp` (cursor bounds and mouse).

The game draws its stone frame at x 14..302, y 15..238 even in 4:3; a CRT's
overscan hid the margins. The wide layout puts the frame on the picture's edges
(left edge at x 0, top bar at y 0, bottom bar one row down, right edge at the
right edge) and grows the map window from (32,32)-(288,217) to (18,17)-(306,218),
LEFT/RIGHT-anchored. No filler art goes in the margins.

The map draw is `func_ovlM_801A103C`; `state = *(u32*)0x80197B18`. In draw order:

| piece | game code | 4:3 layout | wide treatment |
|---|---|---|---|
| terrain | strip loop `0x801A16BC`..`0x801A185C` | 25 strips of 8 rows from the 512x320 CI8 map at `state+0x00` + 512 (a 512-byte TLUT first); scroll `state+0x40`/`+0x44`; x 32..287 under (32,32)-(288,217) | the wide window's scissor, edge-anchored, at `0x801A1694`; extra strips either side and 15 rows above appended at `0x801A1860` |
| sprites | `func_ovlM_8019F83C` (dots, party, cursor, units), `func_ovlM_801A2D84` (location icons) | clipped at x = 0 | rectangle rewritten with signed coordinates (`0x8019F9D4`, `0x801A3298`) |
| shading | loop `0x801A19D8`..`0x801A1B18`, I8 layer at `state+0x2C` | 24 strips at x 32..287, y 32..224 | each strip split: columns 0..63 LEFT from x 18, 64..103 stretched, 104..255 RIGHT to x 306; rows placed by the frame's row map (`0x801A1B1C`) |
| frame | loop `0x801A1D64`..`0x801A1EE0`, CI4 at `state+0x38` (16-colour TLUT, then 288x228) | 38 strips at x 14..302, y 15 + 6n | each strip: filler, columns 0..119 LEFT at x 0..120, columns 120..287 RIGHT at x 152..320; rows 0..17 up 15, rows 186..227 down 1, rows between stretched (`0x801A1EE4`) |
| plate, panel | `func_ovlM_801A26C0(24,22)`, `func_ovlM_801A2410(150,203)` | | rect align LEFT with offset (−14,−15) / RIGHT with offset (+18,+1) (`0x801A1F64`, `0x801A1F74`, reset `0x801A1F80`) |
| fade | block at `0x801A1FC8` | (0,0)-(320,240) | rect align LEFT..RIGHT, scissor to the edges |
| end | `0x801A2120` | | the game's (0,0)-(319,239) scissor back, both alignments reset |

Game logic: the scroll is clamped to `[E + 14, 238 - E] x [15, 124]` after the
game's own `[0, 256] x [0, 124]` clamps (`0x8019B688`, `0x8019C8B4`,
`0x8019D4A4`) and at the start of the draw (`0x801A13C4`). The cursor's bounds
`[40, 272] x [40, 192]` become `[26 - E, 290 + E] x [25, 192]`
(`ogre::map_cursor_bounds`) at the D-pad tests (`0x8019B378`, `0x8019B3A0`,
`0x8019B3D8`), the stick clamps (`0x8019B4B8`, `0x8019B4DC`) and the
clamp-and-store (`0x8019B8F8`, `0x8019B900`, `0x8019B908`). The D-pad overshoots
its bound by up to one 8-pixel step, as in the game. The mouse path clamps the
pointer into the same bounds.

Traps on this screen:

- **A strip loads more than it shows.** Each terrain strip loads 256 texels but
  its rectangle ends at x = 287, so it shows 255 columns. The right extension
  starts at x = 287 with column `scroll + 255`.
- **Art has cut-outs for overlays.** The frame's bottom bar steps down at a
  pillar (columns 128..134) and is transparent where the MONTH/DATE panel sits.
  The split is at column 120 (x 134), so the pillar moves with the panel.
- **Fill gaps from plain art with a wide repeat.** The gap is filled from frame
  columns 48..111 with a `masks = 6` tile (TMEM word 3); a 32-column window shows
  a visible repeat. The filler covers the gap plus one pixel.
- **Measure before stretching.** The shading layer's columns 64..103 have a
  spread of 0 in every row (read from an RDRAM dump), so they stretch without a
  seam.
- **Menus over the screen stay centred.** The R menu and the info windows are
  drawn after the map's draw ends, with the default alignment.
- **The last column and row.** The game's full-screen scissor is (0,0)-(319,239)
  with an exclusive lower-right edge; the RT64 patch's presenter crops one column
  and row (`OGRE_OVERSCAN`, default 1); and RT64 puts a RIGHT-anchored x = 320
  about 1.33 px short of the right edge at 16:9. The frame and the fade draw
  under an extended scissor (`gEXSetScissor`, no bounds clamp) and run 2 px past
  the right edge with a clamped tile, and the map turns the crop off while it is
  drawn wide (`ogre_overscan_crop_override`).

Still 4:3: the map draw's other branch (`state+0x1C4` ≥ 101, `0x801A1088`; the
patched branch is `0x801A13C4`).

## 5. The Organize Screen (`0x06`)

Code: `app/src/organize_widescreen.cpp`, hooks in `config/banks/config-bankS.toml`.
The layout: the cobblestone across the full width, the counters and WAR FUNDS at
the left edge, the name plate at the right edge, the grid centred.

The draw is `func_ovlS_801B7FC0`; `state = *(u32*)0x80196B18`, and `state[0]` is
the screen asset the list uses as segment 6.

- **Background.** `G_DL 0x06002528` (`0x801B8264`) draws one quad from the four
  vertices at `state[0] + 0x24E8`: x = ±160, y = ±120, a 64x64 CI4 tile wrapped
  with TEXTURE scale 0.5 (one period is 4096 in s). The hook at `0x801B8014`
  rewrites x to ±288 and s at the same rate (28608/320 per pixel), shifted by
  16384 to stay in s16, and restores the original values while WIDESCREEN is off.
  Expand's counter-scale then fills any picture up to 1.8 times 4:3.
- **Windows.** One loop over 10 slots at `state + 1616 + 264*i`
  (`0x801BA1B8`..`0x801BA594`) draws each active window: its frame
  (`func_ovlS_801BEC98`, border rectangles plus a parchment fill of triangles), a
  scissor over its inside, then its content callback at `+0`. Slot fields: `+0`
  content, `+12`/`+16` left/top, `+20`/`+24` right/bottom (edges), `+230` active,
  `+238` frame type. `func_ovlS_801CC994(type, ?, content, ...)` creates them.
  The hook at `0x801BA1D0` anchors a window by its content callback: the counters
  `0x801B111C` left, the name plate `0x801B2000` (type 17, sliding in from x 324
  to (208,200)-(296,224)) right, WAR FUNDS `0x801B1644` to the nearer edge (a
  sub-screen creates it at the right, `0x801DF0A4`). It sets a full wide scissor,
  the rectangle, scissor and viewport anchoring, and re-issues the viewport
  `0x80186330`. The hook at `0x801BA584` undoes them. Every other window stays
  centred.
- **Grid and units** are under the ortho projection and stay centred. Tiles the
  game places past x = 0 or x = 320 show in the extension.

Traps on this screen:

- **Anchor by identity, not by position.** The module has about 40
  window-creation calls. A content callback names a panel exactly; a position
  test would also move pop-up menus.
- **Look for triangles inside 2D panels.** The window fills are triangles;
  anchoring only the rectangles moves the borders and leaves the fills behind.

## 6. The mission field cursor (`0x03`)

A 3D screen widens with no draw patch, but its logic still assumes 4:3. Expand
keeps every world point at its 320-wide x, so a screen-space cursor at x < 0 or
x > 320 names visible field, and the game's screen-to-world maths stays correct.

The field cursor is f32 x/y at `state+0x08`/`+0x0C`
(`state = *(u32*)0x801AB870`), moved by `func_ovlN_801BE4E0`. The mover adds the
D-pad delta, applies the field's world limits, then clamps x to [40, 280]
(`0x801BEB7C`..`0x801BEBB8`). At `0x801BEC0C` it compares the clamped position
with the moved one; when the clamp changed it, the delta bytes
(`state+0x06`/`+0x07`) stay set and `func_ovlN_801BECF0` pans the camera by
them, so the clamp is where panning starts. `app/src/mission_widescreen.cpp`
saves x before the clamp (`0x801BEB74`) and re-clamps it to [40 − E, 280 + E]
after it (`0x801BEBC4`), hooks in `config/banks/config-bankN.toml`. The mouse
side (`app/src/mission_cursor.cpp`) uses the same bounds and maps the pointer
over the wide picture.

The cursor is drawn by `func_ovlN_801BBA40(x, y)` through the shared 2D blitter
`func_80176C8C`, which clips every rectangle to 320x240
(`0x80176E20`..`0x80176EDC`). Hooks at the cursor draw's entry and epilogue
(`config-bankN.toml`) set a flag, and the blitter hook at `0x80176E20`
(`config/config.toml`) writes a rectangle that crosses the edge as
`gEXTextureRectangle` with signed coordinates, between `gEXPushScissor` and
`gEXPopScissor` under an edge-anchored `Writer::scissor_full()`. RT64 maps the
game's NONE-origin scissor onto the 4:3 picture for a rectangle that does not
span the width, so unclipping the rectangle alone does not show it.

## 7. The cutscene vignette (`0x0D`)

Some cutscenes darken the picture's edges (the sepia intro movie, New Game
step 1). The backdrop draw carries no darkening (combiner `TEXEL0 * PRIMITIVE`,
primitive white); a separate draw does.

`func_ovlL_8022BE50` (unit L, bankRec14a at `0x8022ACB0`) copies ten vertices
from the cutscene's data, `**(g_step_objects + 0x1A48)` with
`g_step_objects = *(u32*)0x8022A994`, into the frame's vertex buffer at
`*(u32*)0x80197198` (`0x80080998` copies `$a1` to `$a0`, `0x3A0` bytes). It then
emits `G_DL 0x8022AB68` (combiner SHADE, alpha blend, smooth shading), four
`VTX n=4` + `TRI2` border quads and one `VTX n=8` + `TRI2` centre quad. In the
intro movie the outer vertices are at x = ±160, y = ±108 with black at alpha 200,
and the inner rectangle at x = −75..85, y = −55..65 with alpha 10. Expand
counter-scales the projection, so without a patch the ring keeps its 4:3 size and
the extension beside it is bright.

`app/src/cutscene_widescreen.cpp` scales the copied vertices' x by
`(160 + E) / 160` at `0x8022BE80`, right after the copy (hook in
`config/banks/config-bankL.toml`). The cutscene's own data is not changed, so
nothing accumulates across frames. `[cutws] vignette ring widened for <E> px per
side` logs the first widened frame. The module's other overlays already reach
past the wide picture: `func_ovlF_8024138C` writes a ±320 x ±240 quad, and the
iris in `func_ovlF_80240FAC` puts its outer ring at 640 x 480.

## 8. Testing a wide screen

- Start from the title with a battery save in a scratch config
  (`OGRE_PREF_DIR=<dir> OGRE_SAVE=<copy of the save> OGRE_SCENE=title`) and reach
  the screen with an exact tap schedule. The map:
  `OGRE_TAP_MS=1500 OGRE_TAP_SCENE_BUTTON="title:start:5,0x12:a:3"`. The
  Organize Screen:
  `OGRE_TAP_SCENE_BUTTON="title:start:5,0x12:a:3,0x05:none:3,0x05:r:1,0x05:a:1"`.
- Capture with `OGRE_CAPTURE_PRESENT=<prefix> OGRE_CAPTURE_EVERY=<n>` and convert
  the PPMs. Present counts are not seconds: the map appears near present 250,
  about 10.5 s in.
- Push the map cursor to an edge without a mouse:
  `OGRE_MOUSE=on OGRE_MAP_CURSOR_EDGE_FORCE=0x200` (left) or `0x100` (right), with
  `OGRE_MAP_CURSOR_LOG=1` to log the game's cursor x.
- Run once with `OGRE_WIDESCREEN=off` and check the screen is the 4:3 original.
- A single capture can miss a fault that comes and goes. Capture presents a few
  apart (`OGRE_CAPTURE_AFTER=<n> OGRE_CAPTURE_EVERY=5`) and compare a statistic of
  the extension columns across them; an odd stride catches a fault that
  alternates with the triple-buffered framebuffers.
- Run a copy of the exe from a scratch directory while another session may
  build: a running `build-dist/ogrebattle64.exe` blocks its link.
