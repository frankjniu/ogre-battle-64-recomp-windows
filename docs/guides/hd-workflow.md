# HD replacement workflow

The procedure for replacing a piece of the game's art with HD art, and for
making a 2D background fill the widescreen picture. The `mods/backgrounds/`
pack, its backdrops and its HD strips are described in `hd-backgrounds.md`;
RT64's Expand and extended GBI are in `widescreen.md`.

## 1. Pick the mechanism

RT64 replaces a texture by a hash of the bytes it loaded into TMEM
(`tools/RT64/src/common/rt64_tmem_hasher.h`, hash version 5: XXH3 over the
loaded texels, the palette entries they use, the width, the height and the TLUT
mode). A replacement PNG changes what a texture looks like. It cannot change
where or how large the game draws it.

| what changes | mechanism | code needed | example |
|---|---|---|---|
| detail only: same size, same place, the same texels every time | RT64 texture pack entry per hash (section 4) | none | Organize Screen tiles, the map frame, the title's hill |
| an njpeg image (battle, dialogue, cutscene and town backgrounds) | `njpeg/<key>.png` written over the decoder's output, plus HD strips hashed offline | none: `tools/njpeg.py prep`, upscale, `pack` | the 44 njpeg pictures (`hd-backgrounds.md` → "njpeg images by asset") |
| the step-keyed cathedral canvas (njpeg, scene `0x0D`) | pack image written over the decoder's output, plus HD strips | a `backgrounds.txt` line | `01` |
| a full-screen 2D image that must get wider than 4:3, or whose texels are not stable | hook the screen's draw, draw a staged pack image, plus HD strips (section 5) | a hook pair per screen | title, Load Game book, Hugo Report |
| 2D UI that must move to the wide edges | extended-GBI patch of the draw | per screen | world map, Organize Screen (`widescreen.md`) |

Use the texture pack whenever it is enough: it needs no game patch and no
regeneration. A hook is needed when the art changes shape (16:9), when the game
builds the texture at run time (the map's knight sheet is composited into a heap
buffer), or when one load covers pieces whose bytes repeat elsewhere.

## 2. Find the screen and its draw

1. Ask the developer what the screen is, how they reach it, and what a wider
   picture should show (AGENTS rules 1 and 11). Record the answer in
   `docs/scenes.md`.
2. Scene id: run from a save with `OGRE_SCENE_LOG=1` and a tap route, and read
   the `[scene]` lines. The route to the Hugo Report:

   ```sh
   OGRE_NO_AUDIO=1 OGRE_PREF_DIR=<scratch> OGRE_SAVE=<copy of the battery> \
     OGRE_SCENE=title OGRE_SCENE_LOG=1 OGRE_WINDOW=windowed OGRE_CRT=off \
     OGRE_TAP_MS=1500 \
     OGRE_TAP_SCENE_BUTTON="title:start:5,0x12:a:3,0x05:none:3,0x05:r:1,0x05:right:1,0x05:a:1" \
     OGRE_EXIT_AFTER_MS=32000 build-dist/ogrebattle64.exe <rom> > run.log 2>&1
   ```

   Keep `OGRE_SPEED` at 1 for menu taps: at 8 a 150 ms press lasts 1.2 s of game
   time and a menu cursor can move twice.
3. The draw: add `OGRE_DL_TRACE=1` (frame cadence), `OGRE_DL_ANALYZE=1` (counts
   per list) and `OGRE_DL_DECODE=<list number>` (one list decoded; pick a number a
   few seconds into the screen). The background is the run of `SETTIMG` +
   `LOADTILE`/`LOADBLOCK` + `TEXRECT` (or triangles) that covers the screen. Note
   the image address, format, size and TLUT address. The decoder reads every list
   as F3DEX2; check the task's ucode first (`emulator-first.md`). The screens done
   so far use `0x8009F540` (`F3DEX2.fifo 2.08`), which RT64's extended GBI
   supports.
4. The emitter: `tools/guestmap.py <address>` names the unit that owns an
   address. Disassemble that unit in the WSL tree
   (`mips-linux-gnu-objdump -d build/bank<U>.elf`) and search for the setup list's
   address (`addiu ..., <low half>`) or for the command words the draw writes
   (`lui ..., 0xfd48` for a CI8 `SETTIMG`). Read what the function receives, what
   it writes, and where it calls the strip loop. Example: the Hugo Report's setup
   list `0x8023DF68` is referenced by six functions; the one that also loads a
   256-entry TLUT (`0x073FC000`) and calls a strip drawer is the emitter.
5. The original image: take an RDRAM image at the screen
   (`OGRE_DUMP_RDRAM=<path>`, written at the bounded run's exit) and render it:

   ```sh
   tools/rdram.py <dump> image 0x80197E08 --fmt ci8 --palette 0x801AAA08 \
     --width 320 --height 240 -o original.png
   ```

   For an njpeg backdrop use `make bg-extract BG_ID=<id>` (`hd-backgrounds.md`).

## 3. Make the art

The art is the game's own image, outpainted to 16:9 and upscaled with the
Higgsfield connector on the developer's account. Each call costs about 2
credits; check `balance` first. A failed job is refunded.

1. Prepare the input. Measure the image's dark margins (a column and row mean
   luminance profile) and crop them off at the sides you will extend, so the
   outpaint continues the content and not the black. Enlarge the crop 4x with
   Lanczos. Hugo Report: columns 40..279 of 320, 4x to 960x960. Fill holes that
   game objects leave by interpolating each column across the hole; copying rows
   down leaves a step that the outpaint continues.
2. Upload: `media_upload` (filename, `image/png`) returns a presigned URL;
   `curl -X PUT -H "Content-Type: image/png" --data-binary @<file> '<url>'` must
   return 200; then `media_confirm` with `type: image`.
3. Outpaint: `outpaint_image` with `image_id` and `aspect_ratio: "16:9"`
   (`get_cost: true` first). Wait on `results[0].id` with `jobs_wait` and
   download `result_url`. The requested size is not kept: a 960x960 input came
   back 2752x1536.
4. Upscale: `upscale_image` with the outpaint's job id as `image_id`, its width
   and height, and `resolution: "4k"`. 2752x1536 came back 4096x2294.
5. Check the result against the original before using it. Resample to 240 rows,
   take the centre 320 columns, and compare the subject's position and size and
   the channel means over the interior (the Hugo Report is within 6 levels). Look
   at a 1:1 crop for artefacts. Outpaints vary: two of four title outpaints moved
   the hill's ridge 20 rows near the left edge, which shows as a step against the
   game's own hill.
6. A 4:3 variant is needed when the original has framing at its sides that the
   4:3 view should keep. `hugo.png` is the centre 4:3 of the wide image with the
   wide image's outer 44 columns (in 240-row units) cross-faded in over 14
   columns. Without a 4:3 variant, the wide image's centre is shown in 4:3.
7. Files: `mods/backgrounds/<name>.png` (both modes, or 4:3 when a wide one
   exists) and `<name>-wide.png` (WIDESCREEN on). Both are derived from the ROM:
   add them to `.gitignore` beside `title.png`.

Author for about an integer multiple of 240 rows: a screen backdrop's HD strips
are cut at `round(height / 240)` times the game's size (10x for 2294 rows). An
njpeg backdrop's scale is the PNG width over 496.

## 4. Replace ordinary textures with the texture pack

Sprites, portraits, window art and 3D textures that keep their size and place
need only texture pack entries. `tools/hdtex.py` does every step except the
upscale.

1. Dump. Run a route through the screens with `OGRE_DUMP_TEX=<dir>` (the folder
   must exist). RT64 writes one set per hash: `<hash>.v5.rice.json` (the load:
   RDRAM address, format, size, image width, tile), `.tile.json` (tile width,
   height and TLUT mode), `.rice.rdram` (the source bytes; host order, a guest
   byte at `j ^ 3`), `.rice.palette.*` for a CI texture, and `.tmem`. A hash
   appears once, under the first address that loaded it. Screens that need play
   are dumped by the developer from the install folder:

   ```powershell
   mkdir C:\ob64-dump
   $env:OGRE_NO_AUDIO=1; $env:OGRE_DUMP_TEX="C:\ob64-dump"; .\ogrebattle64.exe
   ```

   Dump once with WIDESCREEN off and once with it on: a widened draw can sample
   other pieces of the same image (the map frame's filler tiles).
2. Scan. `tools/hdtex.py scan <dir> -o <inv>` decodes every texture to
   `<inv>/png/<hash>.png` and rebuilds each source image (tile loads from one
   RDRAM image and palette, placed at their `uls`/`ult`; a block load is its own
   image) as `<inv>/images/<key>.png`, with contact sheets `sheet-*.png`.
   `tools/hdtex.py merge <inv> <inv> -o <inv>` joins scans. A render tile with no
   RDRAM load on record (it samples TMEM that another tile's load filled) is
   decoded from `.tmem` and keyed `tmem-…`.
3. Prep. `tools/hdtex.py prep <inv> <key>,<key>… -o in.png [--tile <keys>|all]`
   lays one or more source images out in one upscaler input with an aspect
   between 1:2 and 2:1, enlarged by an integer factor to about 1024 pixels. A
   1-bit alpha edge keeps its colour for 2 texels, then black; soft alpha is
   premultiplied over black. A wrapping texture (`--tile`) is laid out as 3x3
   periods.
4. Upscale `in.png` with `upscale_image` at `4k` (section 3, steps 2 and 4). The
   upscaler refuses inputs as long as 6:1 or 1:4.
5. Pack. `tools/hdtex.py pack in.json <upscaled.png> --folder <name>` cuts every
   hash of those source images at an integer scale (at most 8x), restores the
   alpha, writes `mods/backgrounds/hd/<name>/<hash>.png` and the `hd/rt64.json`
   entries, and keeps the HD master and the original in
   `mods/backgrounds/hd-src/<name>/`. A hash that another folder owns stays with
   it. A wrapping texture's middle period is cross-faded into its neighbours over
   8 texels, so it repeats without a seam.
6. Repack after a draw change or a new route: `tools/hdtex.py repack <inv>
   --folder <name>` matches each master to the scan's source images by format,
   row width and equal texels (not by address: heap images move) and cuts the new
   hashes, including `tmem-…` pieces found inside a master.

RT64 loads only textures listed in `hd/rt64.json`; a PNG without an entry is
ignored. `write_pack` in `tools/backgrounds.py` and `merge_pack` in
`tools/hdtex.py` both keep other folders' entries. The folders made this way are
`ui-organize` (cobblestone and parchment, 64x64 CI4 tiles), `ui-map` (the 288x228
CI4 frame, 38 strips and 9 filler tiles) and `ui-misc` (the title logo, the
MONTH/DATE panel, the Organize grid blocks). `mods/backgrounds/hd/` and `hd-src/`
are gitignored.

Limits of the hash route:

- A texture whose bytes change (an animated palette, a sprite composited at run
  time, a buffer reused for other images) gets a new hash per state.
- Identical bytes share one hash. A flat strip collides with any flat texture of
  its size.
- A large image loaded in pieces is replaced piece by piece; cut the HD art
  along the same pieces.
- The pack loads from `<pack>/hd` only when `mods/backgrounds/backgrounds.txt`
  loads. `OGRE_BG_HD=0` turns it off for an A/B.
- HD detail shows above RESOLUTION 1x with CRT FILTER off. A filter averages the
  frame to one texel per framebuffer pixel, so HD art then only anti-aliases.

## 4b. Replace an njpeg picture

The battle, dialogue, cutscene and town backgrounds are njpeg images; the pack
replaces them by asset (`hd-backgrounds.md` → "njpeg images by asset").
`tools/njpeg.py` does every step but the upscale, from the ROM:

```sh
python tools/njpeg.py prep <rom> -o <dir>        # <name>.png (4x Lanczos input),
                                                 # <name>-orig.png, <name>.json
# upscale <name>.png with upscale_image at 4k (section 3, steps 2 and 4)
python tools/njpeg.py pack <rom> <dir>/<name>.json <upscaled.png>     --capture <OGRE_DUMP_TEX dir> --assume --folder njpeg-<name> --scale 6     --src mods/backgrounds/hd-src/njpeg
```

`<name>` is `b5-<rom>` for a `'B5'` composition (its chunks assembled) and
`s-<rom>` for a single image. `pack` writes `njpeg/<key>.png` per chunk and the
HD strips under `hd/njpeg-<name>/`, and merges `hd/rt64.json`. A chunk found in
a capture gets the captured strip draw, including the bytes a strip reads past
the image; `--assume` gives the others the measured draw without the strips that
read past the image. Before packing, compare the upscale with
`<name>-orig.png`: resampled to the original size it should sit at shift (0, 0)
with channel means within a few levels (the 44 done on 2026-11-01 were within 1.1
levels and 1.8-7.4 mean difference). The upscaler refused one square input
(1596x1596) and one 1:1.95 input once each; a retry passed. The plan runs at
most 8 upscale jobs at once. At 6x the 44 pictures take about 215 MB of strips
(4x would be about 95 MB), and their 4k masters about 500 MB in
`hd-src/njpeg/`.

## 5. Add a screen backdrop

For a screen that needs a hook (section 1, third row):

1. `mods/backgrounds/backgrounds.txt`: `<id> <name> <scene> -`.
2. `app/src/hd_backgrounds.cpp`: add the scene to `kScreenScenes`. The pack then
   loads the image as a screen backdrop, and `widescreen.cpp` adds the scene to
   the Expand set.
3. `app/src/screen_backdrops.cpp`: two hook functions. The begin hook records the
   display-list head (`dl_head`) and the arguments that identify the image. The
   end hook, after the game's draw, checks the commands it expects and the
   image's identity, then writes `draw_backdrop(...)` in place of the game's
   rectangles or strips and moves the head. Identify the image by content (the
   Hugo Report hashes its TLUT), so another image drawn by the same function
   stays the game's. Set the render state the strips need around them (the Hugo
   Report turns the TLUT off and back to RGBA16).
4. `config/banks/config-bank<U>.toml` (or `config/config.toml` for code in
   `RecompiledFuncs`): one `[[patches.hook]]` per site. Do not hook a delay slot;
   put the end hook after the draw call returns and before `jr ra`. Never
   `return` from a hook.
5. Regenerate the unit in the WSL tree and copy it back (`widescreen.md` →
   "Regenerating one bank"), then build (`app-build.md` → "Windows setup").
6. `tools/backgrounds.py`: add the scene's route to `SCREEN_ROUTES` (and to
   `SCREEN_NEEDS_SAVE` when it starts from a save), so `hd <id>` can reach it.
7. Build the HD strips: `make bg-hd BG_ID=<id>` (`hd-backgrounds.md` → "HD
   strips").

## 6. Check once, then stop

Run the screen with WIDESCREEN on and capture a present
(`OGRE_CAPTURE_PRESENT=<prefix> OGRE_CAPTURE_AFTER=<n> OGRE_CAPTURE_EVERY=<m>`).
Present counts are not seconds: read the `[capture]` lines of an earlier run to
place the window. Check the frame rate in the same run (`OGRE_DL_TRACE=1`). Then
the developer looks at it on their machine; when they approve, stop verifying.

## 7. Traps

- On Windows `tools/backgrounds.py` prefers
  `dist/ogre-battle-64-recomp/ogrebattle64.exe` over `build-dist/`. An old `dist/`
  exe runs old code and the dump has no strips. Pass `--app`.
- The outpaint extends whatever touches the edge; crop black margins first.
- Flat strips share hashes with other game textures (section 4).
- A full-width strip needs `gEXSetRectAspect(ADJUST)`, and an added scissor must
  stay (0,0)-(319,239) with edge origins (`widescreen.md` section 2).
  `draw_backdrop` does both.
- Time a slow screen before blaming the art. The Hugo Report ran at 7.5 lists/s
  because of a recompiler-inserted wait in a list walk (`app-build.md` → "A slow
  screen").
- Screen backdrops are staged at `0x80600000`, above the game's 4 MiB. Check an
  RDRAM image of a new screen for writes above `0x80400000` before reusing it.

## 8. Deploy to the play install

The install gets the exe, the images it uses from `mods/backgrounds/`,
`hd/<id>` and `hd/<id>-wide` for those ids, and `backgrounds.txt` and
`hd/rt64.json` filtered to those ids. Leave out the cathedral entry `01` (a test
image). njpeg art goes as `njpeg/` and the `hd/njpeg-*` folders, with their
entries merged into the install's `hd/rt64.json`; keep the install's other
entries as they are. `make dist` leaves `mods/backgrounds/` out, so a release carries none of
this until the developer decides how the art ships (`PLAN.md`).
