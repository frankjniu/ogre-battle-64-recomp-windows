# Widescreen: making a screen fill 16:9

This guide is for widening another screen. It covers how RT64's Expand treats
3D and 2D draws, the extended-GBI rules the patched screens rely on, and the
world map (`0x05`) and the Organize Screen (`0x06`) as worked examples. The
WIDESCREEN setting, its window behaviour and its knobs are in `app-build.md` →
"WIDESCREEN".

## 1. What Expand does, and what it leaves alone

`app/src/widescreen.cpp` turns RT64's `AspectRatio::Expand` on for the scenes in
`expand_scenes()` (`0x03`, `0x0D`, `0x05`, `0x06`, plus every scene the
HD-background pack covers, which includes the title `0x04`, the Hugo Report
`0x08` and the Load Game book `0x12` when their images are present;
`hd-backgrounds.md` → "Screen backdrops"). Expand makes the render target wider by
`aspectRatioScale = target / source` (`rt64_workload_queue.cpp`,
`threadConfigurationUpdate`): `target` is the swap chain's aspect and `source`
is the VI's (4:3). Each side gains `E = 160 * (scale - 1)` pixels in 320-wide
units; for 16:9, `E = 53.3`.

- **3D (projections).** `ProjectionProcessor::processScene` divides a
  projection's x by `scale` when its scissor/viewport covers the whole width and
  is wider than tall. The scene keeps its vertical framing and shows more at the
  sides. The mission (`0x03`) and the dialogue backdrop (`0x0D`) widen this way
  with no game patch. The cutscene vignette drawn over `0x0D` is 2D and needs
  one (section 7).
- **2D (`G_TEXRECT`, `G_FILLRECT`, a projection that does not span the width).**
  `convertFixedRect` (`rt64_framebuffer_renderer.cpp`) keeps the rectangle's
  native size and shifts it by `E`, so the 4:3 picture stays centred and the
  sides stay black. The old comment in `widescreen.cpp` said such draws are
  stretched; the code and the map capture show they are not.
- **2D drawn with triangles (an orthographic projection laid out for
  320x240).** Expand does counter-scale such a projection, which keeps every
  object at its native size and position, centred. The sides show only what the
  game drew past x = 0 and x = 320, usually nothing. The Organize Screen is this
  case: `guOrtho(-160, 160, -120, 120, -10, 100, 1)` and a background quad of
  exactly 320x240.

To classify a screen, run it with `OGRE_DL_ANALYZE=1`: the `[dl-analyze]` line
counts `tri1/tri2` against `texrect`. The world map's list is 2074 commands,
122 texrects, 0 triangles; the Organize Screen's is 220 triangles and 35
texrects, all of them 2D in layout. Triangles alone do not make a screen 3D:
read the projection (`G_MTX` with the projection flag) from an RDRAM dump and
project a few vertices. A quick check is to add the scene with
`OGRE_WS_SCENES=<ids>` and capture a present (`OGRE_CAPTURE_PRESENT`): black
sides around an unchanged picture mean the screen needs a patch.

A 2D screen needs its own draw patched. Ask the developer what the extra width
should show before building (AGENTS rule 1). For the map the choice was more map
with the frame at the edges, over a 4:3 map with extended frame art, or keeping
the pillarbox.

## 2. RT64 extended GBI, as this port uses it

Header: `tools/RT64/include/rt64_extended_gbi.h`. Handlers:
`tools/RT64/src/gbi/rt64_gbi_extended.cpp`. Origin maths: `RDP::movedFromOrigin`
(`rt64_rdp.cpp`) and `convertFixedRect` / `convertViewportRect`.

- **Enable.** `gEXEnable` is the RT64 hook on `G_SPNOOP`: for F3DEX2 the words
  are `E0525464 10000064` (magic `0x525464`, op ENABLE, extended opcode `0x64`).
  Emit it before the first `0x64` command of the list. Check the screen's ucode
  first (AGENTS rule 8); the map's is `F3DEX2.fifo 2.08`.
- **`extAspectRatio` must be `Expand`.** With RT64's default `Original`, LEFT and
  RIGHT origins collapse to the 4:3 picture's centre.
  `set_application_user_config` (`app/src/renderer.cpp`) sets it from `ar_option`.
- **Origins.** A LEFT-origin x is the native x measured from the wide picture's
  left edge. A RIGHT-origin x is written as `x - 320` (in 10.2, `x*4 - 1280`),
  because `movedFromOrigin` adds the colour image's width, and is measured from
  the right edge. A NONE-origin x stays centred, the same as a plain `G_TEXRECT`.
- **Mixed origins stretch.** A rectangle with a LEFT left edge and a RIGHT right
  edge gets its UVs from its native width (`RDP::drawRect`), so its texture
  stretches across the extra `2E`. Use it only on art that is uniform along x.
- **Alignment state.** `gEXSetRectAlign` applies to later plain `G_TEXRECT`s,
  not to `gEXTextureRectangle`, which carries its own origins. `gEXSetScissorAlign` applies to later plain `G_SETSCISSOR`s; set it
  when the game issues its own full-screen scissor after you moved draws to the
  edges, or that scissor clips them. Reset both when the screen's draw ends,
  because the state persists into menus drawn after it.
- **Signed coordinates.** `gEXTextureRectangle` takes signed 16-bit 10.2
  coordinates, so a draw left of native x = 0 is possible; a plain `G_TEXRECT`
  cannot express it.
- **`gEXFillRectangle`** (`G_EX_FILLRECT_V1`; this RT64 has no macro for it)
  is dropped when the raw `lrx < ulx`, before origins apply. For a LEFT..RIGHT
  fill, write `ulx = 0` and the RIGHT-origin `lrx = 0`; fill mode's `|= 3` and
  the scissor finish it.
- **Viewports anchor triangles.** `gEXSetViewportAlign` applies when a viewport
  is next loaded (`RSP::setViewport` moves its x translation from the origin),
  so set it and then re-issue the game's own viewport (`G_MOVEMEM`
  `DC080008 <address>`). RT64 does not counter-scale a projection whose viewport
  has an origin, so triangles under it keep their native layout measured from
  that edge. Put the alignment back to NONE and re-issue the viewport afterwards.
  The Organize Screen's window fills are triangles; anchoring only rectangles
  moved the borders and left the fills behind.
- **Keep full-screen scissors at the game's extent.** RT64 counter-scales a
  projection only when its scissor and viewport cover the frame pair's merged
  scissor (`coversWholeWidth`). The game's full-screen scissor is
  (0,0)-(319,239). An added scissor that reaches x = 320 widens the merged
  scissor, the game's full-screen projections stop qualifying, and the sides show
  stale pixels from older frames. The developer saw this as flicker between the
  wide background and a half-drawn 4:3 image. `Writer::scissor_full()` uses
  (0,0)-(319,239) with LEFT/RIGHT origins.
- **Native-space checks.** RT64 intersects the scissor and the rectangle in
  native coordinates. An extension draw can have no native overlap with an
  edge-anchored scissor; RT64 then tracks an unbounded texcoord range, which is
  harmless.
- **Full-width rectangles stretch.** A NONE-origin rectangle that spans the
  frame pair's scissor width is drawn stretched to the wide picture
  (`coversScissorWidth` in `rt64_framebuffer_renderer.cpp`), and so are tile
  copies. `gEXSetRectAspect(G_EX_ASPECT_ADJUST)` (`0x64000033 00000002`) keeps
  such a rectangle at its native size; put the aspect back to AUTO after. The
  title's backdrop strips need it.
- **Copy mode** divides `dsdx` by 4 in `drawTexRect`. Copy the game's own
  `dsdx`/`dtdy` and cycle type when adding rectangles next to the game's.

## 3. Patching a screen's draw

The bank's generated C is patched with `[[patches.hook]]` entries in its
`config/banks/config-bank<U>.toml`. Each hook is C text inserted before one
instruction, and calls an `extern "C"` function in the app. The shared pieces
are in `app/src/widescreen_gbi.hpp`: guest-memory access, a `Writer` that
appends commands, the extended-GBI encoders, and `widescreen_2d_extension()`,
which `widescreen.cpp` publishes for the patched scenes (`scene_is_patched_2d`)
and which is 0 everywhere else. Add a new screen's id there.

- **The display list.** Every emitter appends at gDisplayListHead,
  `*(u32*)0x800E9BA0`. Read it, write 8-byte commands, store the new head.
- **Cached bases.** Inside a loop the game often keeps the block base in a
  register and writes the next block as `sw ..., 96($t3)`. A hook that appends
  bytes there must also add them to that register (`ctx->r11 += n` for `$t3`), or
  the game overwrites the new commands.
- **Rewriting a loop's output.** After the loop exits, the blocks are contiguous
  and of fixed size (`count * size` bytes before the head). Copy them out,
  validate each block's opcodes, write the expanded blocks from the loop's start,
  then move the head and the cached register. Leave the list untouched when a
  signature does not match, and log it once.
- **Hook placement.** A hook goes before the instruction and after its label,
  so branch targets are fine. Do not hook a delay slot. Do not `return` from a
  hook: the function's own exit calls `recomp_trace_return`, and skipping it
  leaves a frame on the runtime's live call stack. Hook before the function's
  `jr ra` instead, while the registers you need still hold their values.
- **Registers.** `ctx->rN` is the MIPS register number: `$v0` 2, `$a0`..`$a3`
  4..7, `$t0`..`$t7` 8..15, `$s0`..`$s7` 16..23, `$t8` 24, `$s8` 30. Values are
  sign-extended 64-bit.
- **Logic, not only pictures.** A wider view usually needs game logic changed
  too: scroll limits, cursor bounds, and the mouse's window-to-framebuffer
  mapping. The bounds are often immediates in `slti`/`addiu`; a hook after the
  compare returns the register the wide bound gives.
- **The extension value.** Read `E` from RT64's swap chain size (the renderer
  publishes it at each present, `widescreen_note_swapchain`), the way RT64
  derives Expand. SDL's window size is stale in fullscreen. Round up for limits
  that must keep the view on the art, and draw extensions a little wider than
  `E` so the scissor trims them.
- **List capacity.** Measure the screen's list (the last address in an
  `OGRE_DL_DECODE` dump) against its buffer. The map's list uses 13.5 KB of a
  64 KB buffer (`0x8027C140` and `0x8028C120`); the wide patch adds about 9 KB.

### Regenerating one bank

The WSL clone at `/root/ob64` generates the code (`app-build.md` → "Windows
setup"). For bank M alone: copy the edited config into the clone (strip CR),
`rm -rf BankMFuncs`, `tools/N64Recomp/build/N64Recomp
config/banks/config-bankM.toml`, `python3 tools/map_cursor.py` (bank M's
post-generation insertion), and copy `BankMFuncs/` back. Diff the new
`funcs_0.c` against the old one: the hooks should be the only change. Copy back
only the bank you changed when another session's config is also in the clone.

## 4. Worked example: the world map (`0x05`)

Code: `app/src/map_widescreen.cpp` (hook bodies), `config/banks/config-bankM.toml`
(hook sites), `app/src/widescreen.cpp` (turns it on), `app/src/map_cursor.cpp`
(mouse). Every hook is a no-op while `widescreen_2d_extension()` is 0.

The game draws its stone frame at x 14..302, y 15..238 even in 4:3; a CRT's
overscan hid the margins (14 px left, 18 right, 15 rows on top, one at the
bottom). The developer asked for the frame on the picture's edges with the map
filling the inside, and no filler art in the margins (2026-11-01). The wide
layout moves the frame's left edge to x 0, its right edge to the right edge, its
top bar to y 0 and its bottom bar down one row, and grows the map window
(4:3: (32,32)-(288,217)) to (18,17)-(306,218), LEFT/RIGHT-anchored.

The map draw is `func_ovlM_801A103C`; `state = *(u32*)0x80197B18`. In draw order:

| piece | game code | 4:3 layout | wide treatment |
|---|---|---|---|
| terrain | strip loop `0x801A16BC`..`0x801A185C` | 25 strips, 8 rows, from the 512x320 CI8 map at `state+0x00` + 512 (a 512-byte TLUT first); scroll `state+0x40`/`+0x44`; drawn x 32..287 under (32,32)-(288,217) | the wide window's scissor, edge-anchored, at `0x801A1694`; extra strips either side and 15 rows above appended at `0x801A1860` |
| sprites | `func_ovlM_8019F83C` (dots, party, cursor, units), `func_ovlM_801A2D84` (location icons) | clipped at x = 0 | rectangle rewritten with signed coordinates (`0x8019F9D4`, `0x801A3298`) |
| shading | loop `0x801A19D8`..`0x801A1B18`, I8 layer at `state+0x2C` | 24 strips at x 32..287, y 32..224 | each strip split: columns 0..63 LEFT from x 18, 64..103 stretched, 104..255 RIGHT to x 306; rows placed by the frame's row map (`0x801A1B1C`) |
| frame | loop `0x801A1D64`..`0x801A1EE0`, CI4 at `state+0x38` (16-colour TLUT, then 288x228) | 38 strips at x 14..302, y 15 + 6n | each strip: filler, columns 0..119 LEFT at x 0..120, columns 120..287 RIGHT at x 152..320; rows 0..17 up 15, rows 186..227 down 1, rows between stretched (`0x801A1EE4`) |
| plate, panel | `func_ovlM_801A26C0(24,22)`, `func_ovlM_801A2410(150,203)` | | rect align LEFT with offset (−14,−15) / RIGHT with offset (+18,+1) around them (`0x801A1F64`, `0x801A1F74`, reset `0x801A1F80`) |
| fade | block at `0x801A1FC8` | (0,0)-(320,240) | rect align LEFT..RIGHT, scissor to the edges |
| end | `0x801A2120` | | the game's (0,0)-(319,239) scissor back, both alignments reset |

Game logic: the scroll is clamped to `[E + 14, 238 - E] x [15, 124]` after the
game's own `[0, 256] x [0, 124]` clamps (`0x8019B688`, `0x8019C8B4`,
`0x8019D4A4`) and at the start of the draw (`0x801A13C4`). The cursor's bounds
`[40, 272] x [40, 192]` become `[26 - E, 290 + E] x [25, 192]`
(`ogre::map_cursor_bounds`) at the D-pad tests (`0x8019B378`, `0x8019B3A0`,
`0x8019B3D8`), the stick clamps (`0x8019B4B8`, `0x8019B4DC`) and the
clamp-and-store (`0x8019B8F8`, and `0x8019B900`/`0x8019B908` for y). Like the
game, the D-pad overshoots its bound by up to one 8-pixel step. The mouse path
clamps the pointer into the same bounds.

Things the map taught:

- **A strip loads more than it shows.** Each terrain strip loads 256 texels but
  its rectangle ends at x = 287, so it shows 255 columns; in 4:3 the frame hides
  column 287. The right extension had to start at x = 287 with column
  `scroll + 255`, or a dark line appeared.
- **Art has cut-outs made for overlays.** The frame's bottom bar steps down at a
  pillar (columns 128..134) and is transparent above the lower part, where the
  MONTH/DATE panel sits. The first attempt split at column 146 and filled the
  gap from columns 128..159: the pillar stayed behind on the left, and the gap
  showed a row of pillars and black holes. The split is at column 120 (x 134),
  so the pillar moves with the panel.
- **Fill gaps from plain art, and repeat it widely.** The gap is filled from
  frame columns 48..111 with a `masks = 6` tile (TMEM word 3). A 32-column
  window showed its crack pattern three times across a 16:9 gap. The filler
  covers the gap plus one pixel, so it does not show through cut-outs.
- **Measure before stretching.** The shading layer's columns 64..103 have a
  spread of 0 in every row (read from an RDRAM dump), so they stretch without a
  seam; the panel's shadow starts at column ~108, inside the right part.
- **Menus over the screen stay centred.** The R menu and the info windows are
  drawn after the map's draw ends, with the default alignment, and stay 4:3 in
  the middle.
- **The last column and row need three changes.** The game's full-screen
  scissor is (0,0)-(319,239) and the RDP's lower-right edge is exclusive; the
  RT64 patch's presenter crops that column and row (`OGRE_OVERSCAN`, default 1);
  and RT64 puts a RIGHT-anchored x = 320 about 1.33 px short of the right edge
  at 16:9 (its misalignment correction and the expanded target's rounding).
  The frame and the fade draw under an extended scissor (`gEXSetScissor`, no
  bounds clamp) and run 2 px past the right edge with a clamped tile, and the
  map turns the crop off while it is drawn wide
  (`ogre_overscan_crop_override`).
- **Stretched rectangles need `gEXForceUpscale2D`.** The app sets RT64's
  Upscale2D to Original (`renderer.cpp`), which snaps a rectangle's UVs to the
  native pixel; the pillars, stretched by 1.095, then doubled about every
  tenth row. The stretched frame strips force the upscale and reset it.

## 5. Worked example: the Organize Screen (`0x06`)

Code: `app/src/organize_widescreen.cpp`, hooks in `config/banks/config-bankS.toml`.
The developer chose the cobblestone across the full width, the counters and WAR
FUNDS at the left edge, the name plate at the right edge, and the grid centred.

The draw is `func_ovlS_801B7FC0`; `state = *(u32*)0x80196B18`, and `state[0]` is
the screen asset the list uses as segment 6.

- **Background.** `G_DL 0x06002528` (`0x801B8264`) draws one quad from the four
  vertices at `state[0] + 0x24E8`: x = ±160, y = ±120, a 64x64 CI4 tile wrapped
  with TEXTURE scale 0.5 (one period is 4096 in s). The hook at `0x801B8014`
  rewrites x to ±288 and s at the same rate (28608/320 per pixel), shifted by
  16384 to stay in s16, and puts the original values back while WIDESCREEN is
  off. Expand's counter-scale then fills any picture up to 1.8 times 4:3.
- **Windows.** One loop over 10 slots at `state + 1616 + 264*i`
  (`0x801BA1B8`..`0x801BA594`) draws each active window: its frame
  (`func_ovlS_801BEC98`, border rectangles plus a parchment fill of triangles),
  a scissor over its inside, then its content callback at `+0`. Slots: `+0`
  content, `+12`/`+16` left/top, `+20`/`+24` right/bottom (edges, not sizes),
  `+230` active, `+238` frame type. `func_ovlS_801CC994(type, ?, content, ...)`
  creates them. The hook at `0x801BA1D0` anchors a window by its content
  callback: the counters `0x801B111C` left, the name plate `0x801B2000` (type 17,
  sliding in from x 324 to (208,200)-(296,224)) right, WAR FUNDS `0x801B1644` to
  the nearer edge (a sub-screen creates it at the right, `0x801DF0A4`). It sets
  a full wide scissor, the rectangle, scissor and viewport anchoring, and
  re-issues the viewport `0x80186330`. The hook at `0x801BA584` undoes them.
  Every other window stays centred.
- **Grid and units** are under the ortho projection and stay centred. Tiles
  the game places past x = 0 or x = 320 show in the extension.

Things the Organize Screen taught:

- **Anchor by identity, not by position.** The module has about 40 window-creation calls
  across its sub-screens. A content callback names a panel exactly; a position
  test would also move pop-up menus, and a sliding window could change sides.
- **Look for triangles inside "2D" panels.** The first build anchored the
  rectangles only: the borders moved and the parchment stayed, leaving a pale
  box at the old place and the digits drawn over cobblestone.
- **The 320-pixel scissor trap** (section 2) made the background flicker on the
  developer's machine.

## 6. A 3D screen's cursor: the mission field (`0x03`)

A 3D screen widens with no draw patch, but its game logic still assumes 4:3.
Expand keeps every world point at its 320-wide x, so a screen-space cursor at
x < 0 or x > 320 names visible field in the extension, and the game's own
screen-to-world math stays correct there.

The field cursor is f32 x/y at `state+0x08`/`+0x0C`
(`state = *(u32*)0x801AB870`), moved by `func_ovlN_801BE4E0`. The mover adds the
D-pad delta, applies the field's world limits, then clamps x to [40, 280]
(`0x801BEB7C`..`0x801BEBB8`). At `0x801BEC0C` it compares the clamped position
with the moved one; when the clamp changed it, the delta bytes (`state+0x06`/
`+0x07`) stay set and `func_ovlN_801BECF0` pans the camera by them. The clamp is
therefore where panning starts, and with only Expand the cursor stopped at the
old 4:3 edge (developer report, 2026-11-01). `app/src/mission_widescreen.cpp`
saves x before the clamp (`0x801BEB74`) and re-clamps it to [40 − E, 280 + E]
after it (`0x801BEBC4`), hooks in `config/banks/config-bankN.toml`. The mouse
side (`app/src/mission_cursor.cpp`) uses the same widened bounds and maps the
pointer over the wide picture. Bank N's regeneration re-runs
`tools/mission_cursor.py`.

The cursor is drawn by `func_ovlN_801BBA40(x, y)` through the shared 2D blitter
`func_80176C8C`, which clips every rectangle to 320x240 (`0x80176E20..0x80176EDC`),
so the cursor vanished past the 4:3 edge (developer report, 2026-11-01). Hooks
at the cursor draw's entry and epilogue (`config-bankN.toml`) set a flag, and
the blitter hook at `0x80176E20` (`config/config.toml`) writes a rectangle that
crosses the edge as `gEXTextureRectangle` with signed coordinates, between
`gEXPushScissor` and `gEXPopScissor` under an edge-anchored scissor: RT64 maps
the game's own NONE-origin scissor onto the 4:3 picture for a rectangle that
does not span the width, so unclipping the rectangle alone did not show it.
That scissor must be `Writer::scissor_full()`, (0,0)-(319,239). RT64 merges
rectangles' scissors into the frame pair's too (`RDP::drawRect`); a build with
(0,0)-(320,240) widened it, the field's projection stopped qualifying, and the
picture fell back to 4:3 whenever the cursor crossed the edge (developer
report, 2026-11-01). Section 2's 320-pixel trap applies to rectangles, not only
to triangles.

`widescreen_2d_extension()` is published whenever Expand is on, so the map's,
the Organize Screen's, the mission's and the cutscene vignette's hooks all read
the same value; each runs only inside its own scene's code.

## 7. The cutscene vignette (`0x0D`)

Some cutscenes darken the picture's edges; the sepia intro movie (New Game step
1) is one. With only Expand, the darkening stopped at the 4:3 border and the
extension next to it was bright, which the developer saw as a black line on each
side (2026-11-01). The backdrop textures and the backdrop draw carry no
darkening (combiner `TEXEL0 * PRIMITIVE`, primitive white); a separate draw does.

`func_ovlL_8022BE50` (unit L, bankRec14a at `0x8022ACB0`) copies ten vertices
from the cutscene's data, `**(g_step_objects + 0x1A48)` with `g_step_objects =
*(u32*)0x8022A994`, into the frame's vertex buffer at `*(u32*)0x80197198`
(`0x80080998` copies `$a1` to `$a0`, `0x3A0` bytes). It then emits
`G_DL 0x8022AB68` (combiner SHADE, alpha blend, smooth shading), four
`VTX n=4` + `TRI2` border quads and one `VTX n=8` + `TRI2` centre quad. In the
intro movie the outer vertices are at x = ±160, y = ±108 with black at alpha
200, and the inner rectangle at x = −75..85, y = −55..65 with alpha 10. The
projection maps x = ±160 to the 4:3 picture's edges, and Expand counter-scales
it, so the ring keeps its 4:3 size.

`app/src/cutscene_widescreen.cpp` scales the copied vertices' x by
`(160 + E) / 160` at `0x8022BE80`, right after the copy (hook in
`config/banks/config-bankL.toml`). The ring then spans the wide picture with the
4:3 proportions. The cutscene's own data is not changed, so nothing accumulates
across frames. `[cutws] vignette ring widened for <E> px per side` logs the
first widened frame.

The same module's other overlays already reach past the wide picture:
`func_ovlF_8024138C` writes a ±320 x ±240 quad, and the iris in
`func_ovlF_80240FAC` puts its outer ring at 640 x 480 times cos/sin.

## 8. Testing a wide screen

- Start from the title with a battery save in a scratch config
  (`OGRE_PREF_DIR=<dir> OGRE_SAVE=<copy of the save> OGRE_SCENE=title`), and reach
  the screen with an exact tap schedule; for the map,
  `OGRE_TAP_MS=1500 OGRE_TAP_SCENE_BUTTON="title:start:5,0x12:a:3"`.
- Capture with `OGRE_CAPTURE_PRESENT=<prefix> OGRE_CAPTURE_EVERY=<n>`, and
  convert the PPMs. Present counts are not seconds: in these runs the map
  appeared near present 250, about 10.5 s in.
- Push the cursor to an edge without a mouse:
  `OGRE_MOUSE=on OGRE_MAP_CURSOR_EDGE_FORCE=0x200` (left) or `0x100` (right),
  with `OGRE_MAP_CURSOR_LOG=1` to log the game's cursor x.
- Run once with `OGRE_WIDESCREEN=off` and check the screen is the 4:3 original.
- A single capture can miss a fault that comes and goes. Capture a run of
  presents a few apart (`OGRE_CAPTURE_AFTER=<n> OGRE_CAPTURE_EVERY=5`) and compare
  a statistic of the extension columns across them; an odd stride catches a
  fault that alternates with the triple-buffered framebuffers.
- The Organize Screen is reached from the map with
  `OGRE_TAP_SCENE_BUTTON="title:start:5,0x12:a:3,0x05:none:3,0x05:r:1,0x05:a:1"`
  (R opens the map's menu on Organize, A enters it).
- Run a copy of the exe from a scratch directory while another session may
  build: a running `build-dist/ogrebattle64.exe` blocks its link.
