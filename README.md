# Ogre Battle 64: Person of Lordly Caliber — Windows recomp

A native Windows port of the N64 game *Ogre Battle 64: Person of Lordly Caliber*
(USA, Rev A), made by static recompilation with [N64Recomp] and rendered by
[RT64].

**This repository contains no copyrighted game data.** You must supply your own
ROM dump.

## About this fork

This is a Windows fork of
[lfarroco/ogre-battle-64-recomp](https://github.com/lfarroco/ogre-battle-64-recomp).
It builds, tests and releases the Windows executable only. Download it from the
[Releases](https://github.com/frankjniu/ogre-battle-64-recomp-windows/releases)
page.

Since upstream's v0.8.3 the fork adds lower CPU use (the idle thread sleeps; the
start screen uses 7% of a core instead of 170%), a DPI-aware 1080p window with
integer-scaled rendering, seam-free 2D art at high resolution, CRT filters,
cheats, quick save and load, widescreen on more screens, replacement backdrops,
hang and crash reports from double-clicked launches, and a documented Windows
build.

## Features

Every setting is on the start screen and in the `ESC` panel during play, and is
saved in `settings.cfg` or `controls.cfg` beside the executable.

| Feature | What it does |
|---|---|
| GAME SPEED | 1x, 2x, 4x or 8x. During play, `=`/`-` on the keyboard or R3 on a pad change it. |
| RESOLUTION | `NATIVE` (320x240), `2X`, or `AUTO` (the default: the largest whole multiple of 320x240 that fits the window). |
| WIDESCREEN | 16:9 on the mission field, cutscene dialogues, the world map and the Organize Screen. Other screens keep 4:3 with side bars. |
| CRT FILTER | `OFF`, `HYLLIAN` (sharp, lightest), `CRTS` (slight curve) or `LOTTES` (shadow mask, bloom and curve). |
| WINDOW | Windowed or fullscreen. |
| SOUNDS, VOLUME | Sound on or off, and the output volume. |
| CONTROLS | Rebind every N64 button on the keyboard and on an XInput gamepad. |
| MOUSE | The pointer drives the world-map and mission-field cursors. Off by default. |
| CHEATS | MAX GOTH, MAX ITEM COUNTS, and the USA v1.1 GameShark list: QUICK LEVEL GAIN, QUICK SOLDIER PROMOTION, UNLOCK ALL CLASSES, DAY ALWAYS 00, TIME OF DAY. |
| Quick save | `F5` copies the battery save to a quick slot; `F9` twice restores it and restarts the game. |
| Mods | `.nrm` code mods in `mods/`, turned on one by one on the MODS tab. The package ships three, all off: Skip Boot Logos, EXP Overflow and Item Randomizer (experimental). |
| Backdrops | `mods/backgrounds/` replaces dialogue and screen backdrops, with HD detail from an RT64 texture pack. The release does not include a pack; [`docs/guides/hd-backgrounds.md`](docs/guides/hd-backgrounds.md) explains how to make one. |
| Saves | The battery save is `saves/ogrebattle64-us-rev1.bin`. `tools/sramsave.py` imports emulator saves (mupen64plus `.srm`, parallel-n64, DexDrive `.N64`). |
| Crash log | A crash or a hang writes `error.log` beside the executable. |

## System requirements

* **OS:** Windows 10 or 11, 64-bit.
* **GPU:** Direct3D 12.0 (Shader Model 6.0) or Vulkan 1.2, roughly a GeForce GT
  630, Radeon HD 7750 or Intel HD 510 and newer.
* **CPU:** x86-64 with SSE4.1.
* **RAM:** 2 GB.
* **Disk:** about 150 MB, plus the ROM.
* **Game data:** your own Ogre Battle 64 (USA, Rev A) cartridge dump, 40 MB, as
  `.z64`, `.n64` or `.v64`.

A keyboard is enough; an XInput gamepad is optional. No Visual C++
redistributable is needed.

## Playing

Unzip the release and start `ogrebattle64.exe`. The start screen asks for the ROM
once: press `SPACE` to pick it, drag it onto the window, or put it next to the
executable. `ESC` opens the same panel over the running game.

If the game crashes at start, update the graphics driver first. `OGRE_CONSOLE=1`
opens a console with the boot log, and `OGRE_GRAPHICS_API=vulkan` or `d3d12`
pins the graphics backend.

## Building

The full recipe is in [`docs/guides/app-build.md`](docs/guides/app-build.md) →
"Windows setup". In short:

1. Generate the recompiled code from your ROM with `make regenerate` in WSL (it
   needs a MIPS toolchain), and copy the generated directories into the Windows
   checkout. The generated C is not committed.
2. Build with the Visual Studio 2022 Build Tools and clang-cl:
   `make app DIST_OS=windows CMAKE_ARGS="-G Ninja
   -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl"`.
3. Package and check: `make dist DIST_OS=windows`, then
   `tools/smoke-dist.sh`.

The ROM must be the USA Rev A dump (SHA-1
`d26b14bee422a09581ce438d45ba9fec9783fbb8` as `.z64`). `tools/convert_rom.py`
converts a byte-swapped `.n64`.

## Project docs

* [`PLAN.md`](PLAN.md): status and the to-do list.
* [`AGENTS.md`](AGENTS.md): working rules and durable technical facts.
* [`docs/README.md`](docs/README.md): documentation index.

## Directory layout

```
app/        the port's CMake project (the executable)
asm/        splat-generated disassembly
assets/     your ROM (gitignored)
config/     splat, N64Recomp, bank, symbol and RSPRecomp configs
docs/       documentation
mods/       example mods and the backdrop pack
patches/    our patches to N64Recomp, N64ModernRuntime and RT64
tools/      build and diagnostics scripts; RT64 submodule
Makefile    split, link, recompile, build, package
```

## AI disclaimer

This project is mostly an experiment of checking "How far can DeepSeek and Claude Code go?". So far,
the results are quite impressive!

## Legal

Ogre Battle 64 © Quest / Nintendo. This project is for preservation and
interoperability research. Never distribute the ROM or its extracted assets.

## License

The code in this repository is licensed under the GNU General Public License,
version 3. `LICENSE` holds the full text. The app links [N64ModernRuntime]
(GPL-3.0) statically, so the executable as a whole is GPL-3.0.

The license covers the port's own work: the app sources, the configs, the
scripts in `tools/`, the patches and the example mods. It does not cover Ogre
Battle 64 or anything taken from its ROM: the disassembly in `asm/`, the C that
the recompilers generate, and the game's graphics in screenshots under `docs/`
and `debug/`.

Third-party components keep their own licenses: [N64Recomp] and [RT64] are MIT,
and SDL2 is zlib.

[N64Recomp]: https://github.com/N64Recomp/N64Recomp
[N64ModernRuntime]: https://github.com/N64Recomp/N64ModernRuntime
[RT64]: https://github.com/rt64/rt64
