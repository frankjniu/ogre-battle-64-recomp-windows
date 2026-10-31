# Ogre Battle 64: Person of Lordly Caliber — Windows recomp

Static recompilation of the N64 game *Ogre Battle 64: Person of Lordly Caliber*
(USA, Rev A) to a native Windows executable, using the [N64Recomp] toolchain and
the RT64 renderer.

**This repository contains no copyrighted game data.** You must supply your own
ROM dump.

## About this fork

This repository is a Windows fork of
[lfarroco/ogre-battle-64-recomp](https://github.com/lfarroco/ogre-battle-64-recomp).
It builds, tests and releases the Windows executable only. Download the Windows
build from this repository's
[Releases](https://github.com/frankjniu/ogre-battle-64-recomp-windows/releases)
page.

Changes since upstream's v0.8.3 release:

- **Lower CPU use.** The game's idle thread sleeps instead of spinning a core: a
  30-second boot uses about 4 s of CPU instead of 34 s. The start screen uses 7%
  of one core instead of 170%.
- **Sharper picture.** The window opens at 1080p, is DPI aware, and renders at
  the largest whole multiple of 320x240 that fits it. 2D art (dialogue boxes,
  portraits) is sampled at the console's pixel positions, so it shows no seams at
  high resolution.
- **CRT filters:** HYLLIAN, CRTS and LOTTES.
- **Cheats:** MAX GOTH and MAX ITEM COUNTS.
- **Quick save and quick load** with F5 and F9.
- **Replacement dialogue backdrops,** including higher-resolution images through
  an RT64 texture pack. The repository ships one test image;
  [`docs/guides/hd-backgrounds.md`](docs/guides/hd-backgrounds.md) explains how
  to make a pack.
- **Fixes:** audio tasks run in step with the RSP, a stalled game writes a hang
  report, and a double-clicked launch that crashes still writes `error.log`.
- **A documented Windows build** with the Visual Studio 2022 Build Tools and
  clang-cl.

## Features

Every setting is on the start screen and in the `ESC` panel during play, and is
saved in `settings.cfg` or `controls.cfg` beside the executable.

| Feature | What it does |
|---|---|
| GAME SPEED | 1x, 2x, 4x or 8x. During play, `=`/`-` on the keyboard or R3 on a pad change it. |
| RESOLUTION | `NATIVE` (320x240), `2X`, or `AUTO` (the default). |
| WIDESCREEN | 16:9 on the mission field and in cutscene dialogues; other screens keep 4:3 with side bars. |
| CRT FILTER | `OFF`, `HYLLIAN` (sharp, lightest), `CRTS` (slight curve) or `LOTTES` (shadow mask, bloom and curve). |
| WINDOW | Windowed or fullscreen. |
| SOUNDS, VOLUME | Sound on or off, and the output volume. |
| CONTROLS | Rebind every N64 button on the keyboard and on an XInput gamepad. |
| MOUSE | The pointer drives the world-map and mission-field cursors. Off by default. |
| CHEATS | MAX GOTH holds goth at 9,999,999; MAX ITEM COUNTS holds every owned item at 99. |
| Quick save | `F5` copies the battery save to a quick slot; `F9` twice restores it and restarts the game. |
| Mods | `.nrm` code mods in `mods/`, turned on one by one on the MODS tab. The package ships three, all off: Skip Boot Logos, EXP Overflow and Item Randomizer (experimental). |
| Saves | The battery save is `saves/ogrebattle64-us-rev1.bin`. `tools/sramsave.py` imports emulator saves (mupen64plus `.srm`, parallel-n64, DexDrive `.N64`). |
| Crash log | A crash writes `error.log` beside the executable with the fault address and the N64 thread call chain. |

## System requirements

* **OS** — Windows 10 or 11 (64-bit).
* **GPU** — Direct3D 12.0 (Shader Model 6.0) or Vulkan 1.2. The oldest GPUs that
  covers are roughly GeForce GT 630, Radeon HD 7750 (2012) and Intel HD 510.
* **CPU** — x86-64 with SSE4.1.
* **RAM** — 2 GB. The emulated N64 machine commits 512 MB.
* **Disk** — about 150 MB for the app, plus the ROM.
* **Game data** — your own Ogre Battle 64 (USA, Rev A) cartridge dump, 40 MB in
  `.z64`, `.n64` or `.v64`.

A keyboard is enough to play; an XInput gamepad is optional. With no audio device
the game runs silently. No Visual C++ redistributable is required: the package
carries the runtime its shader compiler needs.

The game opens a 1440x1080 window (1920x1080 with WIDESCREEN on) and renders at
the window's pixel size. SETTINGS → RESOLUTION switches to the original 320x240
(`NATIVE`) or `2X`.

If the game crashes as it starts, update the graphics driver first.
`OGRE_CONSOLE=1` opens a console with the boot log, `OGRE_GRAPHICS_API` pins the
backend (`vulkan` or `d3d12`), and a crash writes `error.log` beside the
executable.

## Playing

Unzip the release, then start `ogrebattle64.exe`. The start screen (`OGRE BATTLE
64: RECOMP`, tabs MAIN, MODS, CONTROLS, SETTINGS, CHEATS, DEBUG) asks for the ROM once:
press `SPACE` to pick it, drag it onto the window, or put it next to the
executable. `ESC` opens the same panel over the running game. The battery save is
`saves/` beside the executable.

## Building

The full Windows recipe (Visual Studio 2022 Build Tools with clang-cl, the code
generation in WSL, the build, the package and the traps) is in
[`docs/guides/app-build.md`](docs/guides/app-build.md) → "Windows setup". In
short:

1. Generate the recompiled code from your ROM with `make regenerate` (it needs a
   MIPS toolchain, so it runs in WSL), and copy the generated directories into
   the Windows checkout. The generated C is not committed.
2. Build: `make app DIST_OS=windows CMAKE_ARGS="-G Ninja
   -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl"`.
3. Package and check: `make dist DIST_OS=windows`, then
   `tools/smoke-dist.sh`.

The ROM must be the USA Rev A dump (SHA-1
`d26b14bee422a09581ce438d45ba9fec9783fbb8` as `.z64`). `tools/convert_rom.py`
converts a byte-swapped `.n64`.

## Project docs

* [`PLAN.md`](PLAN.md) — where the port stands and the to-do list.
* [`AGENTS.md`](AGENTS.md) — working rules and durable technical facts.
* [`docs/README.md`](docs/README.md) — documentation index.

## Directory layout

```
app/                   the port's CMake project (the executable)
assets/                your ROM (gitignored; big-endian .z64 expected)
asm/                   splat-generated disassembly
config/                every config the build reads (splat, N64Recomp, banks,
                       symbols, RSPRecomp)
patches/               our patches to N64Recomp, N64ModernRuntime and RT64
mods/                  example mods and the HD backgrounds pack
tools/                 build and diagnostics scripts; RT64 submodule
Makefile               assemble + link + recompile + build + package
```

splat, N64Recomp and RSPRecomp resolve the paths inside a config relative to
that config's directory, which is why the configs carry `../` entry points and
the bank configs set `base_path: ../..`.

## AI disclaimer

This project is mostly an experiment of checking "How far can DeepSeek and Claude Code go?". So far,
the results are quite impressive!

## Legal

Ogre Battle 64 © Quest / Nintendo. This project is for preservation and
interoperability research. Never distribute the ROM or its extracted assets.

[N64Recomp]: https://github.com/N64Recomp/N64Recomp
