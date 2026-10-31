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
  for the mission field, mouse support on the world map and the mission field
  (off by default), mods (`exp-overflow`, `skip-boot-logos`, `item-randomizer`),
  HD dialogue backdrops (`mods/backgrounds/`).
- **Performance:** the game's idle thread sleeps instead of spinning a core
  (`config/config.toml` hook), RT64's queue producers block instead of spinning,
  and a stock run prints no per-task or per-frame log lines. A 30 s boot uses
  about 4 s of CPU.
- **Logging on Windows:** `ogrebattle64.exe > run.log 2>&1` keeps its log, and a
  double-clicked launch still fills `error.log`'s captured sections on a crash.
- **Releases:** the last published release is v0.8.3. None of the 2026-10-30
  work is released; `make release` needs write access to the private data
  repository.

---

## To do

Ordered by how much each item affects a Windows player. Each item says what is
known and what the next step is.

### Robustness

1. **A NULL-handle check in `tools/smoke-dist.sh`.** The package check runs the
    app with inherited handles only. A launch with no standard handles (the
    Explorer case) should assert that `error.log` captures the boot line.
    `main.cpp` reopens handle-less `stdout`/`stderr` on `NUL` as fds 1 and 2,
    and `OGRE_CRASH_TEST=segv` writes the report the check reads. Open problem:
    `explorer.exe <exe>` starts the exe from the running Explorer, so it does
    not inherit the script's environment (`OGRE_CRASH_TEST`, `OGRE_PREF_DIR`);
    a User environment variable set for the launch and removed afterwards is
    the known workaround. Other no-handle launch styles get quarantined by
    Defender. Next step: decide whether the check runs on the hosted runner
    (no Explorer session there) or only locally.
2. **Framebuffer tile-copy freshness.** RT64 serves a tile copy only when the
    source framebuffer was written in the current workload. The writers of two
    leftover framebuffer records (`0x802E6840`, `0x32AF40`) are not identified,
    and a test against `fb->RAMHash` alone is unreliable. `0x2E6840` is one of
    the world map's three 640-wide targets (AGENTS.md). Next step: an
    `OGRE_DL_TRACE` run on the world map from a save, logging each
    `G_SETCIMG` address, to name the writer of `0x32AF40`.

### Features and mods

3. **True HD backgrounds.** `mods/backgrounds/` repaints the backdrop at the
    game's own resolution. Higher detail needs RT64 texture replacement keyed by
    the drawn texture's hash (`TextureCache::loadReplacementDirectory`). The
    four-chunk canvas layout is hardcoded to the cathedral's chunk sizes. See
    `docs/guides/hd-backgrounds.md`.

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
