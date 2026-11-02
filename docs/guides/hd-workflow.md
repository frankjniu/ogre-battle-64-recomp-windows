# HD replacement workflow

This guide is the procedure for replacing a piece of the game's art with HD art,
and for making a 2D background fill the widescreen picture. It names the steps,
the tools, and the traps hit so far. The mechanisms are documented in
`hd-backgrounds.md` (the pack, the njpeg backdrops, the screen backdrops) and
`widescreen.md` (RT64's Expand and extended GBI). The worked examples are the
cathedral (`0x0D` step 2), the title (`0x04`), the Load Game book (`0x12`) and
the Hugo Report (`0x08`).

## 1. Pick the mechanism

RT64 replaces a texture by a hash of the bytes it loaded into TMEM
(`tools/RT64/src/common/rt64_tmem_hasher.h`, hash version 5: XXH3 over the
loaded texels, the palette entries they use, the width, the height and the TLUT
mode). A replacement PNG changes what the texture looks like. It cannot change
where or how large the game draws it.

| what changes | mechanism | code needed | example |
|---|---|---|---|
| detail only: same size, same place, the game's texels are the same every time | RT64 texture pack entry per hash | none | the title's hill (23 strips, `SCREEN_OVERLAYS` in `tools/backgrounds.py`) |
| the picture of a dialogue/cutscene backdrop (njpeg, scene `0x0D`) | pack image written over the decoder's output, plus HD strips | none per backdrop: a `backgrounds.txt` line | the cathedral |
| a full-screen 2D image that must get wider than 4:3, or whose texels are not stable | hook the screen's draw, draw a staged pack image, plus HD strips | a hook pair per screen | title, Load Game book, Hugo Report |
| 2D UI that must move to the wide edges | extended-GBI patch of the draw | per screen | world map, Organize Screen (`widescreen.md`) |

Use the texture-pack route whenever it is enough: it needs no game patch and no
regeneration. A hook is needed when the art changes shape (16:9), when the game
builds the texture at run time (the map's knight sheet is composited into a heap
buffer), or when one load covers pieces whose bytes repeat elsewhere.

## 2. Find the screen and its draw

1. **Ask the developer** what the screen is, how they reach it, and what a wider
   picture should show (AGENTS rules 1 and 11). Write the answer into
   `docs/scenes.md`.
2. **Scene id.** Run from a save with `OGRE_SCENE_LOG=1` and a tap route, and
   read the `[scene]` lines. The route used for the Hugo Report:

   ```sh
   OGRE_NO_AUDIO=1 OGRE_PREF_DIR=<scratch> OGRE_SAVE=<copy of the battery> \
     OGRE_SCENE=title OGRE_SCENE_LOG=1 OGRE_WINDOW=windowed OGRE_CRT=off \
     OGRE_TAP_MS=1500 \
     OGRE_TAP_SCENE_BUTTON="title:start:5,0x12:a:3,0x05:none:3,0x05:r:1,0x05:right:1,0x05:a:1" \
     OGRE_EXIT_AFTER_MS=32000 build-dist/ogrebattle64.exe <rom> > run.log 2>&1
   ```

   Keep `OGRE_SPEED` at 1 for menu taps: at 8 a 150 ms press lasts 1.2 s of
   game time and a menu cursor can move twice.
3. **The draw.** Add `OGRE_DL_TRACE=1` (frame cadence), `OGRE_DL_ANALYZE=1`
   (counts per list) and `OGRE_DL_DECODE=<list number>` (one list decoded; pick a
   number a few seconds into the screen). In the decode, the background is the
   run of `SETTIMG` + `LOADTILE`/`LOADBLOCK` + `TEXRECT` (or triangles) that
   covers the screen. Note the image address, format, size and TLUT address. The
   decoder reads every list as F3DEX2; check the task's ucode before relying on
   it (`emulator-first.md`). The screens done so far all use `0x8009F540`
   (`F3DEX2.fifo 2.08`), which RT64's extended GBI works with.
4. **The emitter.** `tools/guestmap.py <address>` names the unit that owns an
   address. Disassemble that unit in the WSL tree
   (`mips-linux-gnu-objdump -d build/bank<U>.elf`) and search for the setup
   list's address (`addiu ..., <low half>`) or for the command words the draw
   writes (`lui ..., 0xfd48` for a CI8 `SETTIMG`). Read the function: what it
   receives (an image record, a state block), what it writes, and where the
   strip loop is called. The Hugo Report's emitter was found from the setup
   list `0x8023DF68`, which six functions reference; the one that also loads a
   256-entry TLUT (`0x073FC000`) and calls a strip drawer was it.
5. **The original image.** Take an RDRAM image at the screen
   (`OGRE_DUMP_RDRAM=<path>`, written at the bounded run's exit) and render it:

   ```sh
   tools/rdram.py <dump> image 0x80197E08 --fmt ci8 --palette 0x801AAA08 \
     --width 320 --height 240 -o original.png
   ```

   For an njpeg backdrop use `make bg-extract BG_ID=<id>` instead
   (`hd-backgrounds.md`).

## 3. Make the art

The art so far is the game's own image, outpainted to 16:9 and upscaled with the
Higgsfield connector on the developer's account. The developer approved this
route for the title, Load Game and Hugo Report art (2026-11-01). Each call cost
2 credits; check the balance first (`balance`).

1. **Prepare the input.** Measure the image's dark margins first (a column and
   row mean luminance profile). Crop them off at the sides you will extend, so
   the outpaint continues the content and not the black. Enlarge the crop 4x
   with Lanczos. For the Hugo Report: columns 40..279 of 320, 4x to 960x960.
   Fill holes that game objects leave in a render before outpainting
   (the Load Game desk: interpolate each column across the hole; copying rows
   down left a step the outpaint continued).
2. **Upload.** `media_upload` (filename, `image/png`) returns a presigned URL;
   `curl -X PUT -H "Content-Type: image/png" --data-binary @<file> '<url>'`
   must return 200; then `media_confirm` with `type: image`.
3. **Outpaint.** `outpaint_image` with `image_id` and `aspect_ratio: "16:9"`
   (`get_cost: true` first). Wait on the id in `results[0].id` with
   `jobs_wait`, and download `result_url`. The requested size is not kept: a
   960x960 input with 1712x960 requested came back 2752x1536.
4. **Upscale.** `upscale_image` with the outpaint's job id as `image_id`, its
   width and height, and `resolution: "4k"`. 2752x1536 came back 4096x2294.
5. **Check it against the original** before using it. Resample the result to
   240 rows, take the centre 320 columns, and compare with the original: the
   subject's position and size, and the channel means over the interior (the
   Hugo Report was within 6 levels). Look at a 1:1 crop of the upscale for
   artefacts. Two of four title outpaints moved the hill's ridge 20 rows near
   the left edge; the one used keeps it within 2 rows.
6. **A 4:3 variant** is needed when the original has framing at its sides that
   the 4:3 view should keep. `hugo.png` is the centre 4:3 of the wide image with
   the wide image's outer 44 columns (in 240-row units) cross-faded in over 14
   columns. Without a 4:3 variant, the wide image's centre is shown in 4:3.
7. **Files.** `mods/backgrounds/<name>.png` (used in both modes, or in 4:3 when
   a wide one exists) and `<name>-wide.png` (WIDESCREEN on). Both are derived
   from the ROM: add them to `.gitignore` beside the title's.

A screen backdrop's HD strips are cut at `round(height / 240)` times the
game's size (10x for 2294 rows), so author for about an integer multiple of 240
rows. An njpeg backdrop's scale is the PNG width over 496 (`hd-backgrounds.md`).

## 4. Wire a screen backdrop

For a new screen that needs a hook (section 1, third row):

1. `mods/backgrounds/backgrounds.txt`: `<id> <name> <scene> -`.
2. `app/src/hd_backgrounds.cpp`: add the scene to `kScreenScenes`. The pack then
   loads the image as a screen backdrop (240 rows, width rounded to a multiple
   of 4, centred), and `widescreen.cpp` adds the scene to the Expand set.
3. `app/src/screen_backdrops.cpp`: two hook functions. The begin hook records
   the display-list head (`dl_head`) and the arguments that identify the image;
   the end hook, after the game's draw, checks the commands it expects and the
   image's identity, then writes `draw_backdrop(...)` in place of the game's
   texture rectangles or strips and moves the head. Identify the image by
   content (the Hugo Report hashes its TLUT) so another image drawn by the same
   function stays the game's. Set the render state the strips need around them
   (the Hugo Report turns the TLUT off and back to RGBA16).
4. `config/banks/config-bank<U>.toml` (or `config/config.toml` for code in
   `RecompiledFuncs`): one `[[patches.hook]]` per site. Hook an instruction
   that is not a delay slot; the end hook goes after the draw call returns and
   before `jr ra`. Never `return` from a hook.
5. Regenerate the unit in the WSL tree and copy it back (`widescreen.md` →
   "Regenerating one bank"): copy the config with CRs stripped, `rm -rf
   Bank<U>Funcs`, run `tools/N64Recomp/build/N64Recomp
   config/banks/config-bank<U>.toml`, run the unit's post-generation script if
   it has one (the `bank-recomp` target lists them), diff against the checkout
   (the hooks should be the only change), copy back, and build
   (`app-build.md` → "Windows setup").
6. `tools/backgrounds.py`: add the scene's route to `SCREEN_ROUTES` (and
   `SCREEN_NEEDS_SAVE` when it starts from a save), so `hd <id>` can reach it.

## 5. Build the HD strips

```sh
python tools/backgrounds.py hd <id> --app build-dist/ogrebattle64.exe \
  --rom <rom> --save <battery> --dump <scratch dir>
```

It runs the route once per variant (WIDESCREEN off, then on when `-wide`
exists) with `OGRE_DUMP_TEX`, finds every strip loaded from the staging buffer
`0x80600000`, checks its texels against the image the port staged, and writes
`mods/backgrounds/hd/<id>/`, `hd/<id>-wide/` and the merged `hd/rt64.json`.
The result is good when both lines read 100 % of texels matching and N of N
positions with their own hash:

```
plain: 320x240 backdrop at x 0: 40 of 40 staged strips are this image (100.0% ...), 40 of 40 positions with their own hash
wide: 428x240 backdrop at x -54: 60 of 180 staged strips are this image (100.0% ...), 60 of 60 positions with their own hash
```

Other screens staged at the same address on the route (the title, the book)
show up as extra staged strips and are filtered by their texels.

## 6. Check once, then stop

Run the screen with WIDESCREEN on and capture a present
(`OGRE_CAPTURE_PRESENT=<prefix> OGRE_CAPTURE_AFTER=<n> OGRE_CAPTURE_EVERY=<m>`;
present counts are not seconds: read the `[capture]` lines of an earlier run to
place the window, and give the run enough time to reach it). Check the
frame rate in the same run (`OGRE_DL_TRACE=1`). Then the developer looks at it
on their machine; when they approve, stop verifying.

## 7. Replacing ordinary textures (no hook)

For sprites, portraits, window art and 3D textures that keep their size and
place, only the texture pack is needed. `tools/hdtex.py` does every step but
the upscale.

1. **Dump.** Run a route through the screens with `OGRE_DUMP_TEX=<dir>` (the
   folder must exist). RT64 writes one set per hash: `<hash>.v5.rice.json` (the
   load: RDRAM address, format, size, image width, tile), `.tile.json` (the
   tile's width and height and TLUT mode), `.rice.rdram` (the source bytes;
   host order, a guest byte at `j ^ 3`), `.rice.palette.*` for a CI texture,
   and `.tmem`. A hash appears once, under the first address that loaded it.
   Screens that need play are dumped by the developer, from the install
   folder in PowerShell:

   ```powershell
   mkdir C:\ob64-dump
   $env:OGRE_NO_AUDIO=1; $env:OGRE_DUMP_TEX="C:\ob64-dump"; .\ogrebattle64.exe
   ```

   Dump once with WIDESCREEN off and once with it on: a widened draw can
   sample other pieces of the same image (the map frame's filler tiles).
2. **Scan.** `tools/hdtex.py scan <dir> -o <inv>` decodes every texture to
   `<inv>/png/<hash>.png`, and rebuilds each source image (tile loads from one
   RDRAM image and palette, placed at their `uls`/`ult`; a block load is its
   own image) as `<inv>/images/<key>.png`. `sheet-*.png` are contact sheets.
   `merge <inv> <inv> -o <inv>` joins scans. A render tile with no RDRAM load
   on record (address 0; it samples TMEM another tile's load filled) is
   decoded from `.tmem` and keyed `tmem-…`.
3. **Prep.** `tools/hdtex.py prep <inv> <key>,<key>… -o in.png [--tile
   <keys>|all]` lays one or more source images out in one upscaler input with
   an aspect between 1:2 and 2:1, enlarged by an integer factor to about 1024
   pixels. Alpha: a 1-bit edge keeps its colour for 2 texels, then black;
   soft alpha is premultiplied over black. A wrapping texture (`--tile`) is
   laid out as 3x3 periods.
4. **Upscale** `in.png` with `upscale_image` at `4k` (section 3, steps 2 and
   4). The upscaler refused 6:1 and 1:4 inputs and, once, a square one that a
   retry accepted; failed jobs are refunded.
5. **Pack.** `tools/hdtex.py pack in.json <upscaled.png> --folder <name>`
   cuts every hash of those source images at an integer scale (at most 8x),
   restores the alpha (bicubic; 1-bit thresholded; soft alpha divided back
   out), writes `mods/backgrounds/hd/<name>/<hash>.png` and the `hd/rt64.json`
   entries, and keeps the HD master and the original in
   `mods/backgrounds/hd-src/<name>/`. A hash another folder owns stays with it.
   A wrapping texture's middle period is cross-faded into its neighbours over
   8 texels at two edges, so it repeats without a seam.
6. **Repack** after a draw change or a new route: `tools/hdtex.py repack
   <inv> --folder <name>` matches each master to the scan's source images by
   format, row width and equal texels (not by address: heap images move) and
   cuts the new hashes, including `tmem-…` pieces found inside a master.

RT64 loads only textures listed in `hd/rt64.json`; a PNG without an entry is
ignored (`ReplacementDatabase::resolvePaths`). `write_pack` in
`tools/backgrounds.py` and `merge_pack` in `tools/hdtex.py` both keep other
folders' entries.

Done this way (2026-11-01): `ui-organize` (cobblestone and parchment, 64x64
CI4 tiles), `ui-map` (the 288x228 CI4 frame, 38 strips plus 9 filler tiles),
`ui-misc` (the title logo, the MONTH/DATE panel, the Organize grid blocks).

Limits of the hash route:

- A texture whose bytes change (an animated palette, a sprite composited at run
  time, a buffer reused for other images) gets a new hash per state.
- Identical bytes share one hash. A flat strip collides with any flat texture of
  its size; screen backdrops mark such strips (`make_screen_strips_unique`).
- A large image loaded in pieces is replaced piece by piece; cut the HD art
  along the same pieces.
- The pack loads from `<pack>/hd` only when `mods/backgrounds/backgrounds.txt`
  loads (`hd_backgrounds_texture_pack`). `OGRE_BG_HD=0` turns it off for an A/B.
- CRT FILTER averages the frame to one texel per framebuffer pixel, so HD detail
  shows as anti-aliasing only with a filter on (`PLAN.md`, Display item 1).

## 8. Traps hit so far

- **The stale `dist/` exe.** On Windows `backgrounds.py` prefers
  `dist/ogre-battle-64-recomp/ogrebattle64.exe`. An old one loaded the Hugo
  images as njpeg canvases and the dump had no strips. Pass `--app`.
- **Black margins.** The outpaint extends what touches the edge; crop the
  margins first (section 3).
- **Flat strips** share hashes with other game textures (section 7).
- **Scissor and aspect.** A full-width strip needs `gEXSetRectAspect(ADJUST)`,
  and an added scissor must stay (0,0)-(319,239) with edge origins
  (`widescreen.md` section 2). `draw_backdrop` does both.
- **A slow screen** is not the art's fault until checked. The Hugo Report ran at
  7.5 lists/s because of a recompiler-inserted wait in a list walk
  (`app-build.md` → "A slow screen"); time the screen before and after.
- **Staging RAM.** Screen backdrops are staged at `0x80600000`, above the game's
  4 MiB. Check an RDRAM image of a new screen for writes above `0x80400000`
  before reusing it.

## 9. Deploying to the play install

The install gets the exe, `mods/backgrounds/` with the images it uses,
`hd/<id>` and `hd/<id>-wide` for those ids, and `backgrounds.txt` and
`hd/rt64.json` filtered to those ids. Leave out the repository's cathedral
entry (`01`, a test image). `make dist` leaves `mods/backgrounds/` out, so a
release carries none of this until the developer decides how the art ships
(`PLAN.md`, Features item 3).
