# HD backgrounds (`mods/backgrounds/`)

A pack of PNGs that replaces whole backdrops: the njpeg images (battle,
dialogue, cutscene and town backgrounds, by asset), the step-keyed cathedral
canvas of scene `0x0D`, and the screen backdrops of the title (`0x04`), the
Load Game book (`0x12`) and the Hugo Report (`0x08`). The pack writes its image
at the game's resolution; HD detail comes from RT64 replacement strips cut from
the same PNG. The procedure for a new replacement, the art route and the
texture-pack tool `tools/hdtex.py` are in `hd-workflow.md`.

Code: `app/src/hd_backgrounds.cpp` (the pack, the njpeg write, the strip marks),
`app/src/screen_backdrops.cpp` (the screen backdrop hooks),
`tools/backgrounds.py` (extract and HD strips), `tools/njpeg.py` (the offline
njpeg decoder and the asset-keyed pack).

## The pack

```
mods/backgrounds/
  backgrounds.txt     the mapping
  01.png, 01-wide.png the cathedral placeholder (tracked, a test image)
  make-example.py     the script that wrote the placeholder
  title.png, loadgame.png, hugo.png, hugo-wide.png   screen art (gitignored)
  njpeg/<key>.png     one njpeg asset each, at its own size (tools/njpeg.py, gitignored)
  hd/                 RT64 replacement strips and rt64.json (generated, gitignored)
  hd-src/             HD masters kept by tools/hdtex.py (gitignored)
  reference/          extracted originals (gitignored, ignored by the loader)
```

`backgrounds.txt` has one line per backdrop, whitespace separated; `#` starts a
comment:

```
<id>  <name>     <scene>  <step>
01    cathedral  0x0D     2
02    title      0x04     -
03    loadgame   0x12     -
04    hugo       0x08     -
```

- `id` names the image: `<id>.png`, or `<name>.png` if that is missing.
- `scene` is the scene that draws the backdrop (`docs/scenes.md`).
- `step` is the scripted step `D_8018F1C0` for an njpeg backdrop, and `-` for a
  screen backdrop.
- `<id>-wide.png` (or `<name>-wide.png`) is the variant used with WIDESCREEN on.
  Without it the plain image serves both modes.

A missing image is reported and skipped. The port uses the first of these
folders that holds a `backgrounds.txt`:

1. `$OGRE_BG_DIR` (when set, the only folder searched);
2. `<config>/mods/backgrounds` (`<config>` is the executable's folder;
   `app-build.md` → "Config directory");
3. `<config>/../mods/backgrounds`;
4. `<working directory>/mods/backgrounds`.

`OGRE_BG=0` turns the pack off for one run (`[hdbg] pack disabled`), and
`OGRE_BG_HD=0` keeps the pack but skips the HD strips. Every scene in
`backgrounds.txt` joins the WIDESCREEN Expand set.

## Dialogue backdrops (njpeg, scene `0x0D`)

The game decodes four njpeg sub-images and copies each out of a framebuffer in
`func_ovlE_8019976C` (bank unit E; `njpeg-backgrounds.md`). The four copies
tile a 496x384 canvas:

| pass | copy destination | size | canvas rect |
|---|---|---|---|
| 0 | `0x801AAE90` | 320x240 | `(0, 0)` |
| 1 | `0x801D06B0` | 176x240 | `(320, 0)` |
| 2 | `0x801E50D0` | 320x144 | `(0, 239)` |
| 3 | `0x801FB8F0` | 176x144 | `(320, 239)` |

The rects are where the game draws each chunk, measured with a pack whose pixels
encode their own row and column. The bottom chunks start at row 239, over the
top chunks' last row; the asset's chunk headers (ROM `0x7CADB4` and following)
hold `(-229,-235)`, `(90,-235)`, `(-229,4)` and `(90,4)`. Canvas row 383 is never
drawn.

`ogre_njpeg_pass` (next section) runs at the end of each pass copy. When the
pass's asset has no `njpeg/<key>.png`, the dialogue engine's passes fall back to
this step-keyed canvas: the entry is matched by step (at the readback the scene
word still holds the `0x02` loader, so the step is the key and the scene is
checked only when it matches), and the pass's sub-rectangle of the canvas is
written over its pixels as RGBA5551 in the runtime's byte order (`MEM_H` at
`offset ^ 2`).

Authoring:

- The plain image is the whole canvas: 496x384 or a multiple (1984x1536 for 4x).
  The port box-filters it to 496x384 and splits it by the rects above.
- The `-wide` image is 16:9 (for example 1920x1080). The port scales it
  uniformly to the canvas width and anchors it to the canvas bottom
  (`to_canvas_fit`), so circles stay round. Stretching a 16:9 image to the
  canvas would show it 29 % narrow. The round rose window in `01-wide.png` is the
  aspect check.
- The game's own letterbox at the top and bottom of the dialogue screen is part
  of the scene and is not filled.
- The image is chosen by the WIDESCREEN setting, not by the live aspect, because
  the backdrop is assembled in the `0x02` loader before `0x0D` expands. The log
  tells them apart: `applied 'cathedral' (01 wide)` against `(01)`.

Scene `0x0D` expands with or without the pack: the game's 496x384 backdrop has
content that the 4:3 view crops.

### Extracting the original

```sh
make bg-extract                              # mapping 01, the cathedral
make bg-extract BG_ID=02                     # another mapping
tools/backgrounds.py extract 01 --step 6     # another step of scene 0x0D
tools/backgrounds.py extract --all           # a plain boot; keep every dump
tools/backgrounds.py extract 01 --from <dir> # file an existing dump
tools/backgrounds.py list                    # print the mappings
```

The tool runs the app with `OGRE_BG_DUMP=<dir>`, which writes one
`original-step<NNNN>.png` per assembly before the pack replaces anything, and
files it as `mods/backgrounds/reference/<id>-<name>-original.png`. It is a live
capture of the readback. (This guide said until 2026-11-01 that only the game's
microcode can decode the sub-images; `tools/njpeg.py` decodes every njpeg asset
from the ROM, next section.)

## njpeg images by asset (`njpeg/<key>.png`)

Four records carry the same njpeg module, each with its own state pointer
(`tools/hd_backgrounds.py` lists the call sites):

| copy | unit | state pointer | decode setup | readback |
|---|---|---|---|---|
| dialogue engine (`0x02`/`0x0D`) | bank E, record 0 | `0x8019A680` | `func_ovlE_80199A08` | `func_ovlE_8019976C` |
| record 10 (battle `0x0E`, mask `0x3C00`) | bank C | `0x801D0700` | `func_ovlC_801CE084` | `func_ovlC_801CDDE8` |
| record 10b | bank C | `0x801F0570` | `func_ovlC_801EF4A4` | `func_ovlC_801EF208` |
| title module | `RecompiledFuncs` | `0x801BA6F0` | `func_801B7874` | `func_801B75D8` |

Bank code is outside the `.nrm` mod hook map, so `tools/hd_backgrounds.py`
places two calls in each copy's generated code; `make bank-recomp` and
`make recomp` re-apply them:

- `ogre_njpeg_source(rdram, STATE)` just before the CPU Huffman decode
  `func_8008B250(state[0x6C], state[0x68])`. `state[0x6C]` points at the
  compressed stream's `'HUFF'` tag; the asset's key is FNV-1a 64 over its first
  64 bytes. The bytes come from the ROM, so the key is the same on every
  machine, and `tools/njpeg.py list` prints it (75 assets, 75 keys).
- `ogre_njpeg_pass(rdram, STATE)` after the pass counter store
  (`sb $v0, 0x7E($v1)`). If `njpeg/<key>.png` exists and has the pass's size,
  its texels replace the pass's (`state[0x74]`, +8 on the single-image path).
  Otherwise the dialogue engine's pass goes to the step-keyed canvas above.

The PNG is the asset at its own size with RGBA5551-exact values, written by
`tools/njpeg.py pack`, which also cuts the HD strips. Because the port writes
exactly these texels, the tool computes RT64's hash of every strip offline
(`rt64_hash`, a port of `rt64_tmem_hasher.h` version 5). Checked on the
cathedral: the tool reproduced all 116 strip hashes of a capture from its RDRAM
bytes, and in a run with a pack built from a cathedral master, RT64 loaded
exactly the 116 hashes the tool had computed.

A capture (`OGRE_DUMP_TEX`) of the same pipeline is still needed once per draw
layout: it gives the rows each strip loads, and the bytes past the image that a
240-row chunk's last strip reads (one row more: a heap block header, the next
chunk's record and its first texels). `pack` matches each chunk to the captured
image by comparing texels with the offline decode, and overlays the pack's
texels on every captured address.

`OGRE_BG_LOG=1` prints `[hdbg] njpeg pass state=… key=… WxH at …` per pass and
`[hdbg] applied njpeg <key>.png …` when the pack replaces it. `OGRE_BG_DUMP=<dir>`
also writes the game's own pixels of each asset once per run, as
`njpeg-<key>-<w>x<h>.png`.

## Screen backdrops (title, Load Game, Hugo Report)

These screens do not use njpeg. Hooks on their own draws show the pack image
behind the game's logo, menu, windows, text and sprites (hooks in
`config/config.toml` and `config/banks/config-bankK.toml`). The image is
box-filtered to 240 rows with its aspect kept, its width rounded to a multiple
of 4, and centred on the 320-wide screen; a 16:9 image fills the widescreen
sides, and its centre 4:3 is what a 4:3 window shows. The port stages it as
RGBA16 at `0x80600000` (free RDRAM above the game's 4 MiB) and draws it in
strips of as many rows as fit in TMEM's 4 KB, as extended-GBI rectangles at
signed coordinates under `gEXSetRectAspect(ADJUST)`. RT64 stretches a
NONE-origin rectangle that spans the whole scissor width
(`coversScissorWidth`), so without the ADJUST aspect the image shows 4/3 too
wide.

### The title (`0x04`)

The title module (streamedC, ROM `0x1CE040` → `0x80197B90`, compiled in
`RecompiledFuncs`) draws its backdrop in `func_8019D568`, layer by layer through
`func_80198ECC(record, x, y)`; `state = *(u32*)0x801B80E0`:

| layer | record | image | at |
|---|---|---|---|
| sky | `state+0x00` | RGBA16 320x90 | (0, 0) |
| clouds | `state+0x24` | RGBA32 320x106 | (0, 24) |
| clouds | `state+0x48` | RGBA32 320x160 | (0, 80) |
| soldiers | `state+0x90 + 0x24*D_801BA558` | CI8 72x48, 3 frames | (22, 127) |
| hill | `state+0x6C` | RGBA32 200x112 | (0, 126) |

Between the layers it draws three fog overlays, `func_8019C5D4(0/2/1)`: white
rectangles over a wrapped 64x64 I8 image. With a pack image:

- The pack image is drawn at the sky's `func_80198ECC` entry.
- The rectangles of the sky and both cloud layers become `G_SPNOOP`s. Their
  texture loads stay, because each fog group inherits the tile the layer before
  it left (the renderer's `OGRE_FOG` repair depends on that).
- The hill stays the game's: the soldier sprite is black below their feet, and
  the hill, drawn after it, hides that block. The hill's 23 strips get HD
  replacements cut from the same PNG with the hill's own alpha
  (`SCREEN_OVERLAYS` in `tools/backgrounds.py`).
- Under Expand, `extend_fog` continues each fog row past its ends with
  rectangles of the same chunk width and `s`. Rows at screen y 139 and below are
  not extended on the left, because the game's hill hides its own fog there.

The image keeps the game's geometry: the soldiers draw at their native place,
and the hill's ridge in the PNG must meet the game's hill.

### The Load Game book (`0x12`)

The book screen is 3D. `func_801A9C68` sets the viewport `0x801B8D10`, the
projection (`func_801A8FB4`) and the setup list `0x801B8CA0`, then draws the
object list `func_801A6BE8`: object 0 is the desk (a 15-vertex wood frame, a CI8
32x64 tile that wraps along `s`), objects 1 and 2 are the book's soft shadows (an
RGBA32 16x32 image), and the book follows. Expand alone shows more desk, black
slivers at both edges (the frame ends at screen x -47 and 369), and the cover's
edges.

With a pack image the backdrop is drawn before the setup list with 1-cycle,
opaque, no-Z state, and the desk object's `G_DL` becomes a `G_SPNOOP`. The
shadows, the book and the text stay the game's, so the shadows fall on the new
desk.

### The Hugo Report (`0x08`)

The map's R menu opens the Hugo Report (descriptor `0x8018FC64`, records 14 and
30). Its backdrop is a CI8 320x240 image record (`+3` texel size code 1,
`+4`/`+6` width and height, texels at `+8`, the RGBA16 TLUT after them).
`func_ovlK_802349AC(record)` (unit K, bankRec14d) writes `G_DL 0x8023DF68`
(1-cycle, TLUT RGBA16, point filter, combiner `TEXEL0 * PRIMITIVE + ENVIRONMENT`),
SETPRIMCOLOR white, the TLUT load and TEXTURE on (0x50 bytes), then
`func_ovlK_8022B9A0(record, 0, 0)` draws 40 strips of 6 rows.

The hook after the strip call (`0x80234B4C`) keeps the first 0x50 bytes and
writes the pack backdrop over the strips, with the TLUT off around them. It acts
only when the record is CI8 320x240 and its TLUT hashes to `0x1E1F0BCE` (FNV-1a
over the 512 bytes in guest order). `[hdbg] hugo report: image … left alone`
logs a mismatch. The Events, Miscellany and Tips pages that draw another image
through the same function keep the game's.

## HD strips (`mods/backgrounds/hd/`)

The game draws an njpeg backdrop with the F3DEX2 ucode `0x8009F540` as textured
quads, one `G_LOADTILE` strip per quad, from four RGBA16 images: `0x80243E28`
(320x240), `0x80269660` (176x240), `0x8027E090` (320x144) and `0x802948C0`
(176x144). A strip is 320x6 or 176x11 texels (5 or 10 new rows plus one overlap
row); the cathedral draws 116 strips per frame. A screen backdrop's strips are
loaded from the staging buffer `0x80600000`.

RT64 replaces each strip by the hash of its texels.
`make bg-hd BG_ID=<id>` (`tools/backgrounds.py hd <id>`) builds the replacements:

```sh
make bg-hd                                          # 01, the cathedral
make bg-hd BG_ID=02 BG_ROM=<rom>                    # the title
make bg-hd BG_ID=03 BG_ROM=<rom> BG_SAVE=<battery>  # the Load Game book
make bg-hd BG_ID=04 BG_ROM=<rom> BG_SAVE=<battery>  # the Hugo Report (a save on the map)
tools/backgrounds.py hd 01 --scale 4                # force 4x
tools/backgrounds.py hd 01 --dump <dir>             # keep RT64's texture dumps
tools/backgrounds.py hd 01 --from <dir>             # rebuild from a dump without running
tools/backgrounds.py hd 01 --app build-dist/ogrebattle64.exe   # pick the exe
```

1. It runs the app on the mapping's route (`SCREEN_ROUTES` for a screen
   backdrop) with the pack applied and `OGRE_DUMP_TEX` set, once with WIDESCREEN
   off and once with it on when a `-wide` image exists.
2. From RT64's dump it reads each strip's hash, source address and rows
   (`<hash>.v5.rice.json`) and size (`.tile.json`).
3. It checks each strip's texels against the image the port wrote (the same box
   filter and the same marking pass as the app) and stops when fewer than 98 % of
   the texels match, which is what a different chunk layout produces. It also
   counts the strip positions that have their own hash.
4. It resamples the PNG with Lanczos to an integer scale (njpeg: PNG width / 496;
   screen backdrop: `round(height / 240)`), cuts each strip's rectangle, and
   writes `hd/<id>/<hash>.png`, `hd/<id>-wide/<hash>.png` and `hd/rt64.json`,
   keeping other folders' entries.

A good run reports 100 % of texels matching and N of N positions with their own
hash:

```
plain: 320x240 backdrop at x 0: 40 of 40 staged strips are this image (100.0% ...), 40 of 40 positions with their own hash
wide: 428x240 backdrop at x -54: 60 of 180 staged strips are this image (100.0% ...), 60 of 60 positions with their own hash
```

Other screens staged at `0x80600000` on the route show up as extra staged strips
and are filtered by their texels. Rebuild the strips after changing a PNG,
`chunk_rect` or the marking passes.

Two strips with the same texels would share one HD image. The marking passes
make every strip unique: `make_strips_unique` (njpeg) flips the lowest blue bit
of one texel in the third row of each strip that repeats an earlier one, a row
no other strip loads; `make_screen_strips_unique` (screen backdrops) does the
same to the mark row's last texel of a flat strip, which would otherwise share
its hash with any flat game texture of its size. Both are mirrored in the tool.
The changed texel is drawn only when the HD strips are not loaded.

The app loads `<pack>/hd` as an RT64 texture pack at renderer start and logs
`[hdbg] texture pack <dir>`. The strips use RT64's `stall` operation: RT64
decodes a strip before the frame that first draws it, so no strip shows at game
resolution (116 cathedral strips cost 16.1 ms once). `--operation stream`
decodes on background threads with no wait, and a strip draws at game resolution
until its PNG is ready.

## Verifying

```sh
OGRE_NO_AUDIO=1 OGRE_LAUNCHER_KEYS=space OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 \
  OGRE_BG_LOG=1 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=20000 "$APP" "$ROM"
```

`OGRE_SCENE=new-game OGRE_STEP=2` reaches the cathedral in a few seconds.
`OGRE_LAUNCHER_KEYS=space` passes the start screen, which shows whenever a `.nrm`
mod is installed. `OGRE_BG_LOG=1` prints one line per pass and per applied
chunk:

```
[hdbg] pass dest=0x801AAE90 320x240 step=2 scene=0x7FFF next=0x02 -> cathedral
[hdbg] applied 'cathedral' (01) step=2 chunk (0,0) 320x240 to 0x801AAE90
```

`OGRE_BG_DEBUG=1` paints each pass a solid colour instead of the pack image
(320x240 red, 176x240 green, 320x144 blue, 176x144 yellow), so a capture shows
the four-pass composition. The composed backdrop is drawn a few seconds after
scene `0x0D` starts; an RDRAM image at the scene's first frame holds the raw
njpeg chunks.

## Limits

- The step-keyed canvas knows only the cathedral's four-chunk 496x384 layout
  (`[hdbg] … no canvas rect` for others). The asset-keyed `njpeg/` images have no
  layout limit.
- An asset's HD strips need a capture of a pass drawn the same way (the same
  pipeline, chunk size and buffer); the last strip of a 240-row chunk hashes
  bytes past the image, which a different heap layout changes.
- The pack replaces the backdrop only, not the dialogue box or the portraits.
- The HD strips need one capture run per mapping and per variant, and a run
  reaches only the backdrops its route assembles.
- `make_strips_unique` assumes the draw's strip layout (6 rows every 5 for a
  320-wide chunk, 11 every 10 for a 176-wide one); the tool's hash count reports
  a backdrop drawn another way.
