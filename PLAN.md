# Ogre Battle 64: Person of Lordly Caliber — Windows recomp

A native Windows port of **Ogre Battle 64: Person of Lordly Caliber** (USA, Rev A)
built by static recompilation: `N64Recomp` translates the ROM's MIPS code to C,
which is compiled against `N64ModernRuntime` and rendered by RT64. It is a "bring
your own ROM" project; no ROM data is in the repository.

**Scope: Windows only** (developer, 2026-10-30). The browser, macOS and Linux
builds still exist in the tree but are not maintained or verified.

Read first: [`AGENTS.md`](AGENTS.md) (working rules and durable facts). Build and
debug: [`docs/guides/app-build.md`](docs/guides/app-build.md). Index:
[`docs/README.md`](docs/README.md). This file is the to-do list; keep it current
and delete items when they are done.

---

## Where the port stands

The game runs end to end on Windows: publisher stills, title, New Game opening,
world map, missions and battles, tutorial, shop, Organize Screen, credits,
ending and the attract loop.

- **Recompilation:** 99.05% of the ROM code span `0x001000..0x2B8B70` is
  compiled (the one gap is data). 44 streamed records in 34 bank units; no
  un-recompiled module is on any reachable path. `tools/recompcov.py` measures it.
- **Renderer:** RT64 on D3D12. The window opens 1080 points tall (1440x1080, or
  1920x1080 with WIDESCREEN on), the process is per-monitor DPI aware, and
  RESOLUTION defaults to `auto` (the largest integer multiple of 320x240 that fits
  the window's pixels; 9x on a 200%-scaled 5K display). MSAA is experimental
  (`OGRE_DISPLAY_EXPERIMENTS=1`).
- **Audio:** the game's own audio microcode is recompiled and runs.
- **Saves:** cartridge battery (SRAM) works; Controller Pak menus render.
- **Player features:** launcher and in-game `ESC` overlay (MAIN, MODS, CONTROLS,
  SETTINGS, DEBUG), rebindable keyboard and pad, GAME SPEED 1/2/4/8, WIDESCREEN
  for the mission field, CRT FILTER (HYLLIAN, CRTS, LOTTES), CHEATS (MAX GOTH,
  MAX ITEM COUNTS), quick save and quick load of the battery save (F5/F9),
  mouse support on
  the world map and the mission field (off by default), mods (`exp-overflow`,
  `skip-boot-logos`, `item-randomizer`),
  replacement dialogue backdrops with HD strips (`mods/backgrounds/`, RT64
  texture pack in `mods/backgrounds/hd/`).
- **Performance:** the game's idle thread sleeps instead of spinning a core
  (`config/config.toml` hook), RT64's queue producers block instead of spinning,
  and a stock run prints no per-task or per-frame log lines. A 30 s boot uses
  about 4 s of CPU.
- **Logging on Windows:** `ogrebattle64.exe > run.log 2>&1` keeps its log, and a
  double-clicked launch still fills `error.log`'s captured sections on a crash
  (`tools/smoke-dist.sh` check 5 launches through Explorer to prove it).
- **Releases:** v0.9.0 (2026-10-31) is the last published release, a Windows
  zip on this fork (`frankjniu/ogre-battle-64-recomp-windows`, tag at
  `22390aa`). It carries all the 2026-10-30/31 work. The app reports 0.9.0
  (`recomp::Version` in `app/src/main.cpp`, which the mods'
  `minimum_recomp_version` is checked against). Releases are Windows-only and
  built on this machine; the fork's Release workflow is disabled
  (`docs/guides/app-build.md` → "Releases").

---

## To do

Ordered by how much each item affects a Windows player. Each item says what is
known and what the next step is.

### Display

1. **CRT FILTER and the HD detail.** The filters read the frame averaged to one
    texel per framebuffer pixel, so with a filter on, RESOLUTION AUTO and the HD
    background strips only add anti-aliasing. This is the usual CRT behaviour.
    Next step: ask the developer whether a filter should keep the horizontal
    HD detail (sample the HD frame across a scanline instead of the averaged
    texel), and whether the rows need tunables (scanline strength, mask
    strength, curvature on or off), which today are the libretro defaults.
2. **A vertical line at the centre of the frame in a cutscene dialogue.** The
    developer's screenshot (2026-10-31; fullscreen at 12x, WIDESCREEN on, CRT
    FILTER `crts`) shows a one-column dip at game x 160 that crosses both
    dialogue boxes and the backdrop. A 9x windowed run with the same settings
    does not show it, with the filter on or off. Next step: the developer turns
    CRT FILTER off on the screen that shows the line; if the line stays, capture
    the fullscreen frame with `OGRE_CAPTURE_PRESENT`.

### Features and mods

2. **HD backgrounds beyond the cathedral.** `mods/backgrounds/` writes a
    backdrop at the game's resolution, and `tools/backgrounds.py hd` (`make
    bg-hd`) cuts RT64 replacement strips from the same PNG, one per `G_LOADTILE`
    strip hash (`docs/guides/hd-backgrounds.md`). Only the cathedral (scene
    `0x0D`, step 2) is mapped, and its art is the placeholder pair. The canvas
    layout is hardcoded: four chunks 320/176 wide and 240/144 tall, the right
    chunks at column 320 and the bottom chunks at row 239. The tool stops when a
    backdrop's strips do not match that layout. Next step: the developer names
    the next backdrop (scene and step) and supplies art; map it in
    `backgrounds.txt`, run `make bg-extract` and `make bg-hd`, and generalise
    `chunk_rect` if the check fails.

### Parked

- The `ESC` overlay does not pause the game (developer's choice).
- Gamepad rebinding is verified by code reading only.
- `0x801936A9`, the community's Chaos Frame address, is in the saved flags block
  but has no static read or write in this ROM.

---

## Technical facts worth keeping

- **ROM.** Big-endian `.z64`, SHA-1 `d26b14bee422a09581ce438d45ba9fec9783fbb8`.
  `tools/convert_rom.py` converts a byte-swapped `.n64`; the app accepts `.z64`,
  `.n64` and `.v64`. Entry `0x80070C00`; main segment ROM `0x1060` ↔
  `0x80070C60`; BSS `0x800AEDB0..0x800E9C20`; `main` at `0x8007F880`.
- **libultra** is bridged by name: `config/symbols/symbol_addrs.txt` names the
  libultra functions so the runtime's native services replace the game's copy.
- **Streamed code** is plain MIPS DMA'd to fixed RAM. Records that share a RAM
  range are separate bank units, so a call into the range compiles as
  `LOOKUP_FUNC` and the runtime's DMA-driven bank map picks the resident module.
  Compiling a swappable range into the unit that calls it binds the call to one
  bank at build time (AGENTS.md rule 4).
- **Scene graph.** `func_80075BC0` fills `D_800AF028` with 25 scene accessors;
  39 store sites write the pending scene; the scripted step table has 1693
  entries. `tools/scenemap.py` extracts it from the ROM in seconds.
- **Display lists** go to RT64's own GBI interpreters. RSPRecomp compiles only
  the njpeg and audio microcodes.
- **Threads.** N64 threads are native threads. Thread 1 (`func_8007F8E4`) is the
  boot thread and then the idle loop; thread 3 (`func_80071EB0`) is the game
  flow; thread 4 (`func_8008AFE0`) is the frame pump.
- **Mouse.** The world map cursor is `state+0x52/+0x54` (`state =
  *(u32*)0x80197B18`), written at the cursor draw `0x801A1C48` by
  `app/src/map_cursor.cpp`; the mission field cursor is f32
  `state+0x08/+0x0C` (`state = 0x800E91D0`), written at `L_801BEC04` by
  `app/src/mission_cursor.cpp`. Both hooks are re-inserted by `make bank-recomp`
  (`tools/map_cursor.py`, `tools/mission_cursor.py`).
