# Ogre Battle 64 Windows recomp: status and to-do

The port targets Windows only (developer, 2026-10-30). The browser, macOS and
Linux builds are still in the tree but are not maintained or verified.

Read [`AGENTS.md`](AGENTS.md) first. Build and debug:
[`docs/guides/app-build.md`](docs/guides/app-build.md). Index:
[`docs/README.md`](docs/README.md). This file is the to-do list: delete an item
when it is done.

## Status

The game runs end to end on Windows: publisher stills, title, New Game opening,
world map, missions and battles, tutorial, shop, Organize Screen, credits,
ending and the attract loop.

- **Recompilation:** 99.05% of the ROM code span `0x001000..0x2B8B70` is
  compiled; the one gap is data. 44 streamed records in 34 bank units, and no
  uncompiled module is on a reachable path (`tools/recompcov.py`).
- **Renderer:** RT64 on D3D12, with a per-monitor DPI-aware window 1080 points
  tall and RESOLUTION `auto` by default. MSAA is experimental
  (`OGRE_DISPLAY_EXPERIMENTS=1`).
- **Audio:** the game's own audio microcode is recompiled and runs.
- **Saves:** the cartridge battery (SRAM) works; the Controller Pak menus
  render.
- **Release:** v0.9.1 (2026-11-01), a Windows zip on
  `frankjniu/ogre-battle-64-recomp-windows`. The app reports 0.9.1
  (`recomp::Version` in `app/src/main.cpp`). Releases are built on this machine;
  the fork's Release workflow is disabled (`docs/guides/app-build.md` →
  "Releases").

## To do

Ordered by how much each item affects a Windows player.

### Display

1. **CRT FILTER and HD detail.** The filters read the frame averaged to one
   texel per framebuffer pixel, so with a filter on, RESOLUTION AUTO and the HD
   strips only add anti-aliasing. Next step: ask the developer whether a filter
   should keep the horizontal HD detail (sample the HD frame along a scanline),
   and whether the filters need tunables (scanline strength, mask strength,
   curvature), which today are the libretro defaults.
2. **A vertical line at the centre of a cutscene dialogue.** The developer's
   screenshot (2026-10-31; fullscreen at 12x, WIDESCREEN on, CRT FILTER `crts`)
   shows a one-column dip at game x 160 across both dialogue boxes and the
   backdrop. A 9x windowed run with the same settings does not show it. Next
   step: the developer turns CRT FILTER off on that screen; if the line stays,
   capture the fullscreen frame with `OGRE_CAPTURE_PRESENT`.
3. **Widescreen on more 2D screens.** The world map (`0x05`) and the Organize
   Screen (`0x06`) are wide (`docs/guides/widescreen.md`). On the map the frame
   sits on all four edges and the mouse cursor is clamped to the play area.
   Checked at 16:9 windowed: all four edges covered, both scroll ends, the D-pad
   cursor at both corners, WIDESCREEN off unchanged. Not checked: the mouse
   pointer on the map (the clamp is code-read only), the location plate and the
   fade, 21:9 and fullscreen. The developer reported scrolling to the bottom as
   "a little weird sometimes"; the unclamped mouse cursor is the candidate
   cause. The map draw's other branch (`state+0x1C4` ≥ 101, `0x801A1088`) stays
   4:3. The Organize name plate (`0x801B2000`) at the right edge needs a unit
   selected to check. The mission field cursor now reaches the wide edges
   before the camera pans; built, not run in a mission. Next step: the developer
   checks the map (frame, plate, mouse cursor, bottom scroll), the name plate and
   the mission cursor at both edges, and names the next screens to widen.
4. **`OGRE_CAPTURE_TARGET` segfaults at its first capture** (exit 139, empty
   `error.log`). `OGRE_CAPTURE_PRESENT` works. Suspect: the render-target
   readback in `rt64_present_queue.cpp` copies an `R16G16B16A16_UNORM` target
   into a footprint declared `R8G8B8A8_UNORM`. Next step: match the footprint and
   the buffer size to `ogreCapture.targetFormat`, then capture once on the map.

### Features and mods

5. **HD backdrops beyond the cathedral.** `mods/backgrounds/` writes a backdrop
   at the game's resolution, and `make bg-hd` cuts RT64 replacement strips from
   the same PNG (`docs/guides/hd-backgrounds.md`). Only the cathedral (scene
   `0x0D`, step 2) is mapped, and its art is a placeholder. The canvas layout is
   hardcoded to the cathedral's four chunks, and the tool stops when a backdrop's
   strips do not match it. Next step: the developer names the next backdrop
   (scene and step) and supplies art; map it in `backgrounds.txt`, run
   `make bg-extract` and `make bg-hd`, and generalise `chunk_rect` if the check
   fails.
6. **Title, Load Game and Hugo Report backdrops.** These three screens draw a
   16:9 pack image behind the game's own logo, menu, windows and book
   (`app/src/screen_backdrops.cpp`; hd-backgrounds.md → "Screen backdrops"). The
   art is AI-outpainted and upscaled from the game's own backdrops, so
   `title.png`, `loadgame.png`, `hugo.png` and `hugo-wide.png` are gitignored and
   only in this checkout. The developer approved the Hugo Report at 16:9
   (2026-11-01). Not checked: 21:9, fullscreen, picking a slot from the book, the
   Hugo Report at 4:3 and its Events, Miscellany and Tips pages. Next step: the
   developer decides whether the art ships (it is derived from the ROM) and how;
   `make dist` leaves `mods/backgrounds/` out.
7. **HD art, in order of return.** `tools/hdtex.py` replaces any texture whose
   texels are the same each time it loads, with no game patch, and
   `tools/njpeg.py` replaces the njpeg backgrounds by asset
   (`docs/guides/hd-workflow.md`). HD detail shows above RESOLUTION 1x with CRT
   FILTER off (item 1).
   1. **Shared UI art** (made 2026-11-01): the Organize Screen's cobblestone,
      parchment and grid blocks, the map's stone frame and MONTH/DATE panel, the
      title logo. Next step: the developer looks at them in game.
   2. **Mission field and battles**, where most play time goes. No texture dump
      exists yet; the battle backgrounds are probably njpeg terrains (item 4).
      Next step: the developer plays one mission with a battle and a dialogue
      with `OGRE_DUMP_TEX` set (command in item 4, which the same run serves);
      the scan then lists the field terrain, portraits, unit sprites and windows
      to rank.
   3. **World map painting**: a 512x320 CI8 image (`state+0x00`;
      `state = *(u32*)0x80197B18`). Each terrain strip loads 256 texels from the
      scroll position, so every scroll offset has new hashes and a pack cannot
      reach it. Next step: draw the terrain in fixed 64x32 tiles from the hook in
      `func_ovlM_801A103C` so each tile keeps one hash, after item 3 settles.
   4. **njpeg backgrounds** (2026-11-01, in the play install, not yet seen by
      the developer): the pack keys them by asset (`njpeg/<key>.png`, hooks in
      all four njpeg copies), `tools/njpeg.py` decodes the 75 assets from the
      ROM, and all 44 pictures (10 `'B5'` compositions, 34 single 320x240
      images: 29 terrains and 5 interiors) are upscaled and packed at 6x
      (`mods/backgrounds/hd/njpeg-*`, about 215 MB; 4x would be about 95 MB).
      The cathedral's strips come from a capture and match exactly. Every other
      picture uses the assumed strip draw (114 of the cathedral's 116 hashes
      without a capture); a 240-row chunk's last strip reads past the image and
      needs a capture. Next step: the developer plays a battle and a few story
      dialogues from the install folder with
      `$env:OGRE_NO_AUDIO=1; $env:OGRE_BG_LOG=1; $env:OGRE_BG_DUMP="C:\ob64-njpeg"; $env:OGRE_DUMP_TEX="C:\ob64-dump"; .\ogrebattle64.exe 2> njpeg.log`;
      that confirms the terrains are the battle backgrounds, checks the
      record-10 draw, and gives the bytes for the last strips
      (`tools/njpeg.py pack --capture`). Open: bank C's njpeg copies have no
      `njpeg_readback.py` source fix (watch for a stale chunk in battle
      backgrounds), and the foreground sprites drawn over a backdrop (the
      cathedral's front statue and candles) are ordinary textures for
      `tools/hdtex.py`.
   5. **Unit sprites**: many small pixel-art frames per class and palette, and an
      AI upscale changes the pixel-art look. Next step: one class's frames as a
      trial for the developer to judge.
   6. **Text** is composited into buffers at run time (`func_801A8124`), so a
      hash cannot name a glyph. Parked.

### Parked

- The `ESC` overlay does not pause the game (developer's choice).
- Gamepad rebinding is verified by code reading only.
- `0x801936A9`, the community's Chaos Frame address, has no read or write in
  this ROM; the real byte is `0x801936C9` (`docs/symbols.md`).
