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
  repository (see "Releases" below).

---

## To do

Ordered by how much each item affects a Windows player. Each item says what is
known and what the next step is.

### Player-facing

1. **Check HD on the world map, the mission field and in battle.** Presented
   frames were compared at `native` and `auto` for the intro, publishers, title,
   attract loop and the New Game courtyard movie, and the developer approved
   them. The 3D scenes and the map unit sprites were not captured at HD. RT64's
   framebuffer tile copies (the map unit head sprite path) are the most likely
   place for an HD-only defect. Next step: the developer plays those scenes at
   the default resolution and saves a screenshot of anything wrong.
2. **MSAA.** Still behind `OGRE_DISPLAY_EXPERIMENTS=1`. A run with it showed
   artifacts that were never characterised. Next step: capture one 3D screen at
   MSAA off and `4x` and decide whether to fix or drop the row.
3. **Credits screen (scene `0x11`) background artifacts.** The scrolling credits
   play; some njpeg/S2DEX2 backgrounds show artifacts. Not investigated. Needs a
   save near the ending.
4. **GAME SPEED tops out near 4x.** At the title the display-list rate is 29 /
   59 / 119 / 111 per second at 1x / 2x / 4x / 8x. The limit is downstream of
   RT64's workload and present queues (the GPU workload or the present). Next
   step: find which one, then decide whether presents can be dropped at speed.
5. **The speed badge is not drawn in fullscreen and hides when the pointer is
   over it.** It is a second borderless SDL window; fullscreen has no room for
   it and SDL2 has no click-through hit test. A badge that survives both has to
   be drawn inside RT64's frame.

### Robustness

6. **Latent static backlog.** `tools/stubmap.py report` classifies 986 static
    dispatch targets (130 missing-function candidates, 152 bank-selection
    candidates); the main unit makes 951 direct calls into swappable RAM across
    236 targets (`make cross-bank-check`). `cross_bank.py dispatch --only` fixes
    one target at a time. 288 indirect `jalr` sites are outside any static
    measurement; a run's stub log is the only check.
7. **Audio auto-response shim.** The runtime answers the audio queue
    `0x800C49E8` with a dummy message when it blocks
    (`[dbg] audio-response queue … queuing dummy response`). Firing the real
    `OS_EVENT_AI` when a buffer drains would let it be deleted.
8. **A NULL-handle check in `tools/smoke-dist.sh`.** The package check runs the
    app with inherited handles only. A launch with no standard handles (the
    Explorer case) should assert that `error.log` captures the boot line. Use an
    Explorer launch; other no-handle launch styles get quarantined by Defender.
9. **A hang dump with no periodic cost.** The `[snap]` queue snapshot is off by
    default (`OGRE_SNAP`), so a hang prints nothing. A frame-counter-triggered
    dump (no frame advanced for ~3 s) would keep the diagnosis.
10. **`tools/dlgaps.py` assumes a 1500 ms period.** A stall that stretches the
    period reads as "not periodic". Estimate the period from the spike spacing.
11. **Framebuffer tile-copy freshness.** RT64 serves a tile copy only when the
    source framebuffer was written in the current workload. The writers of two
    leftover framebuffer records (`0x802E6840`, `0x32AF40`) are not identified,
    and a test against `fb->RAMHash` alone is unreliable.

### Releases

12. **Ship the 2026-10-30 work.** A release needs the generated-code bundle in
    the private data repository regenerated for the commit being built
    (`make release`; the hosted Windows job refuses a bundle whose recorded
    commit differs). The `gh` account on the developer's Windows machine
    (`frankjniu`) has pull access to the public repository only. Next step:
    give it access to the data repository, or run `make release` where the
    access exists. The Windows job's smoke test now fails on an empty log, which
    the 2026-10-30 stdio fix provides.

### Features and mods

13. **True HD backgrounds.** `mods/backgrounds/` repaints the backdrop at the
    game's own resolution. Higher detail needs RT64 texture replacement keyed by
    the drawn texture's hash (`TextureCache::loadReplacementDirectory`). The
    four-chunk canvas layout is hardcoded to the cathedral's chunk sizes. See
    `docs/guides/hd-backgrounds.md`.
14. **Item randomizer.** Map pickups and battle rewards are randomized and
    verified. Open: the `+0x02` used-count correction has never run in game
    (no acquisition inherited a non-zero count); hooks on streamed-bank
    functions would let the mod hook the grant routine directly
    (`docs/notes-bank-hooks.md`). A hook on `func_8016B774` crashes the save
    load.
15. **Controller Pak copy/backup** (battery ↔ Pak `Save`/`Load`/`Erase`). The
    screen renders; the device half (`osPfs*`, a flat 32 KiB `.mpk`) is
    implemented; the copy flow is deferred. Low priority (developer).

### Parked

- Audio above 1x game speed is untested.
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
