# Building, running and debugging the Windows port

The app (`app/`) compiles the recompiled game code against N64ModernRuntime,
links the RT64 renderer, and boots the game from the player's ROM. The game code
is generated in WSL, because Windows has no MIPS toolchain, and the app is built
natively with clang-cl. The browser, macOS and Linux builds still exist in the
tree and are not maintained.

Commands are Git Bash unless a block says PowerShell or WSL. The examples use
two shell variables:

```sh
APP=build-dist/ogrebattle64.exe
ROM=/c/path/to/ogre64.z64
```

Every game run sets `OGRE_NO_AUDIO=1` (AGENTS.md rule 14).

## Windows setup

### 1. Compiler, CMake, Ninja and make

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --add Microsoft.VisualStudio.Component.VC.CMake.Project"
choco install make
```

The developer's machine has MSVC 14.44.35207, Windows SDK 10.0.26100.0,
clang-cl 19.1.5, and the CMake and Ninja that ship with the Build Tools. Git
Bash is the shell.

### 2. Generate the game code in WSL

`RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/` and `app/src/bank_funcs.inc` are
generated from the ROM and gitignored. Generate them in WSL (Ubuntu 24.04):

```sh
# WSL
sudo apt-get install build-essential cmake ninja-build clang lld \
  binutils-mips-linux-gnu gcc-mips-linux-gnu python3-venv python3-pip git zip
git clone /mnt/c/Users/<you>/Development/ogre-battle-64-recomp ~/ob64
cd ~/ob64
mkdir -p assets && cp <rom> assets/ogre64.z64
# USA Rev A, SHA-1 d26b14bee422a09581ce438d45ba9fec9783fbb8
python3 -m venv tools/venv && tools/venv/bin/pip install 'splat64[mips]'   # 0.50.1
git clone https://github.com/N64Recomp/N64Recomp.git tools/N64Recomp
git -C tools/N64Recomp checkout ffb39cd
git -C tools/N64Recomp submodule update --init --recursive
git -C tools/N64Recomp apply ../../patches/n64recomp-ob64.patch
cmake -S tools/N64Recomp -B tools/N64Recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tools/N64Recomp/build --target N64RecompCLI RSPRecomp RecompModTool
make regenerate
make example-mods
```

`make example-mods` writes `build/mods/*.nrm`, which `make dist` requires.

The WSL tree also holds everything that needs the ROM or the MIPS ELFs: `asm/`,
`build/*.elf`, `assets/` and `mips-linux-gnu-objdump`. Run the offline tools
that read them (`scenemap.py`, `arenamap.py`, `guestmap.py`, `stubmap.py`,
`recompcov.py`, `elfcheck.py`, `cross_bank.py`) there.

### 3. Copy the generated code into the Windows checkout

```sh
# WSL, from ~/ob64
W=/mnt/c/Users/<you>/Development/ogre-battle-64-recomp
rm -rf "$W"/RecompiledFuncs "$W"/Bank*Funcs "$W"/RspFuncs
cp -r RecompiledFuncs Bank*Funcs RspFuncs "$W"/
cp app/src/bank_funcs.inc "$W"/app/src/
mkdir -p "$W"/build/mods && cp build/mods/*.nrm "$W"/build/mods/
```

Regenerate and copy again whenever `config/`, `asm/` or the Makefile's dispatch
list changes. A WSL clone carries only committed state, so copy uncommitted
edits to `Makefile`, `config/` and `tools/` across first.

### 4. Third-party trees

```sh
git submodule update --init --recursive                    # RT64
git clone https://github.com/N64Recomp/N64ModernRuntime.git tools/N64ModernRuntime
git -C tools/N64ModernRuntime checkout 589bbf018a3e6d3646ddf7de1e7919f1b7e99bb1
git -C tools/N64ModernRuntime submodule update --init --recursive
git -C tools/N64ModernRuntime apply --ignore-whitespace ../../patches/n64modernruntime-ob64.patch
git -C tools/N64ModernRuntime/N64Recomp apply --ignore-whitespace ../../../patches/n64modernruntime-n64recomp.patch
git -C tools/RT64 apply --ignore-whitespace ../../patches/rt64-ob64.patch
git -C tools/RT64/src/contrib/plume apply --ignore-whitespace ../../../../../patches/rt64-plume-ob64.patch
git -C tools/RT64/src/contrib/plume apply --ignore-whitespace ../../../../rt64-plume-sdl.patch
python tools/patchcheck.py                                  # must print PASS
```

A Windows checkout gives the RT64 tree CRLF line endings, so `git apply` needs
`--ignore-whitespace` for hunks with no final newline.

### 5. Build, package, smoke-test

```sh
make app DIST_OS=windows CMAKE_ARGS="-G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl"
make dist DIST_OS=windows
DIST_OS=windows /usr/bin/bash tools/smoke-dist.sh
```

`make app` builds a static SDL2 (`make sdl2-static`) and then the app in
`build-dist/`. `make dist` reuses that configured directory and writes
`dist/ogre-battle-64-recomp/`. Run `make app` with the `CMAKE_ARGS` before the
first `make dist`: a `build-dist/` configured without them uses `cl.exe`, which
fails.

### 6. Traps on this machine

- **`VsDevCmd.bat` fails on a long `PATH`** (`'vswhere.exe' is not recognized`).
  Capture its environment from PowerShell with a minimal `PATH`:

  ```powershell
  $env:PATH = 'C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem;C:\Program Files (x86)\Microsoft Visual Studio\Installer'
  cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 && set' > vsenv.txt
  ```

  Import the `NAME=value` lines into Git Bash (convert `PATH` with
  `cygpath -p`), and prepend these Build Tools directories: `VC\Tools\Llvm\x64\bin`,
  `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` and
  `Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja`.
- **`bash` resolves to WSL** once the VS `PATH` is first. Run repo scripts with
  `/usr/bin/bash`.
- **`make` from PowerShell runs recipes in `cmd.exe`** when no `sh.exe` is on
  `PATH`. Append Git's `usr\bin` at the end of `PATH`, not the front: its
  `link.exe` would shadow the MSVC linker. `tools/smoke-dist.sh` check 5 also
  needs `C:\Windows\System32\WindowsPowerShell\v1.0` on `PATH`.
- **`python3` may be the Microsoft Store stub.** `tools/smoke-dist.sh` skips it;
  `make bg-extract` and `make bg-hd` need a real `python3` first on `PATH`.
- **`build-dist/ogrebattle64.exe` does not start in place** until
  `dxcompiler.dll` and `dxil.dll` are copied beside it from `build-dist/RT64/`.
- **`wsl -d Ubuntu -- bash -c '...'` expands `$VAR` on the Windows side.** Use
  `wsl -d Ubuntu --exec bash -c '...'` or a script file.

### 7. Shell launches, Defender, per-thread CPU

`"$APP" "$ROM" > run.log 2>&1` keeps its log. To test a launch with no standard
handles (a double-click), start the exe with `explorer.exe <exe>` or
`explorer.exe <shortcut.lnk>`; Explorer uses its own environment, so `OGRE_*`
variables from the shell do not reach the game. Scripted launches that cleared
the handles (`DETACHED_PROCESS|CREATE_NO_WINDOW`) or injected threads got the
exe quarantined by Defender.

The runtime names its threads ("N64 Thread n", "Gfx Thread", "RT64 Workload",
…), so `GetThreadDescription` plus `GetThreadTimes` attribute CPU per thread.

## Regenerating the recompiled code

`make regenerate` (WSL) runs the pipeline in its required order:

| step | output |
|---|---|
| `make resplit` | `asm/`, `assets/*.bin`, `ogrebattle64.ld`, `config/symbols/undefined_*_auto.txt` |
| `make` | `build/ogrebattle64.elf` |
| `make bank-recomp` | `Bank*Funcs/`, `app/src/bank_funcs.inc` (34 bank units) |
| `make recomp` | `RecompiledFuncs/`, with the cross-bank dispatch |
| `make rsp-recomp` | `RspFuncs/` (njpeg and audio microcode) |

`bank-recomp` must run before `recomp`, because the cross-bank dispatch sees
only the records in `bank_funcs.inc`. `make bank-recomp` also runs the ELF/ROM
check and `cross_bank.py check-banks`, and re-applies the generated-C insertions
(`tools/njpeg_readback.py`, `hd_backgrounds.py`, `map_cursor.py`,
`mission_cursor.py`). `make recomp` re-applies `hd_backgrounds.py` too, for the
title module's njpeg copy in `RecompiledFuncs`. `make resplit` always leaves `asm/1CE040.s` and
`asm/40E80.s` modified; do not commit or revert them.

## Third-party patches

`tools/N64ModernRuntime` and `tools/RT64` are pinned upstream checkouts that
carry the project's changes as uncommitted work. The tracked copy is
`patches/*.patch` plus `tools/rt64-plume-sdl.patch`.
`python tools/patchcheck.py` (`make patch-check`) fails when a tree and its
patch differ; `make patch-fix` rewrites stale patches (review
`git diff patches/` before committing). `tools/release-build.sh` runs the check.
Regenerate the runtime patch with the nested N64Recomp excluded:
`git -C tools/N64ModernRuntime diff HEAD -- . ':(exclude)N64Recomp'`. Keep
`.gitattributes`' `*.patch eol=lf` and `--ignore-whitespace`.

## Build details

- clang-cl under Ninja: `cl.exe` rejects the GCC-style flags the app, the
  runtime and RT64 use. `app/CMakeLists.txt` keys its flag groups on
  `CMAKE_CXX_COMPILER_ID`.
- SDL2 2.32.10 (`tools/SDL2-static/`) and the CRT are static
  (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`).
- The exe is a GUI-subsystem image with a `WinMain` forwarder in `main.cpp`.
- The app, the recompiled code and the runtime build with
  `-fno-omit-frame-pointer`, so a debugger unwinds across a bridge to the guest
  caller.

## Running

```sh
OGRE_NO_AUDIO=1 "$APP" "$ROM"
```

The ROM is taken from the argument, `OGRE_ROM`, a ROM in the app's folder or
`<app>/roms/`, the stored copy, `assets/`, then the start screen (`SPACE` or a
click opens a file picker; a ROM can be dropped on the window). The ROM is
validated by XXH3 hash and copied into the config directory. The start screen
also appears when a mod is installed. `OGRE_LAUNCHER=1` forces it.

### Console output and logs

A double-click opens no console. A terminal run attaches to the parent console,
and a redirected stream keeps its target. A launch with no standard handles
reopens fds 1 and 2 on `NUL` so the crash log can capture them.
`OGRE_CONSOLE=1` allocates a console.

A stock run prints boot lines and errors only. `OGRE_RSP_LOG=1` adds the `[rsp]`
task lines and `OGRE_DL_TRACE=1` the per-display-list lines.

### Config directory

The config directory is the executable's folder: `saves/<game id>.bin`,
`mods/`, `mods.json`, `mod_config/`, `controls.cfg`, `settings.cfg` and
`error.log`. `OGRE_PREF_DIR=<dir>` overrides it. A read-only install falls back
to `%APPDATA%\ogrebattle64\`. The boot log prints the resolved path.

### Paths under `/tmp`

Several defaults are POSIX paths: the console file `/tmp/ogre-console.txt`, the
bare `dump`/`snap`/`save` outputs, and `OGRE_RAM_KEY` output with no
`OGRE_RAM_KEY_OUT` or `OGRE_PREF_DIR`. The Windows CRT opens `/tmp/...` as
`C:\tmp\...`, which must exist; Git Bash's `/tmp` is `%TEMP%`. Use explicit
`C:/tmp/...` paths.

### Closing the window

The close button and EXIT GAME end with `crash_log::flush()` and `_Exit(0)`,
because nothing joins the game threads. The saving thread is joined first, so
the battery save is safe.

## Packaging

### `make dist`

```sh
make dist DIST_OS=windows       # -> dist/ogre-battle-64-recomp/
make dist-zip DIST_OS=windows   # -> dist/ogre-battle-64-recomp-windows.zip
```

The package holds `ogrebattle64.exe`, `dxcompiler.dll`, `dxil.dll`, the MSVC
runtime DLLs they import (`msvcp140.dll`, `vcruntime140.dll`,
`vcruntime140_1.dll`, and `msvcp140_atomic_wait.dll` when present),
`README.txt` (from `packaging/README-dist.txt`), `LICENSE.txt`,
`SDL2-LICENSE.txt`, `mods/*.nrm` and an empty `mod_config/`. `make dist` fails
when a runtime DLL, the generated code or `build/mods/*.nrm` is missing.

### Smoke test

```sh
DIST_OS=windows /usr/bin/bash tools/smoke-dist.sh [dist-dir]
```

Call `/usr/bin/bash` directly; `make smoke` can find WSL's `bash`. It checks
(1) the package files and at least one `mods/*.nrm`, (2) that every imported DLL
is shipped or a system DLL (`tools/pe_imports.py`), (3) that the exe reaches
`main` with `OGRE_SMOKE=1` within `SMOKE_TIMEOUT` (default 90 s), (4) that
`OGRE_CRASH_TEST=segv` writes an `error.log` holding the `[boot]` line, and
(5) the same report from an Explorer launch through a `.lnk` with
`--crash-test=segv` (no standard handles; skipped without Explorer).

### Crash and hang reports — `error.log`

`app/src/crash_log.cpp` mirrors fds 1 and 2 into rings and installs the fatal
handlers at the top of `main()`. A crash writes `<config>/error.log`: the
reason, the faulting instruction and address (host module and offset), the RDRAM
base, the N64 thread, the last recompiled function per thread, and the last 200
stderr and 100 stdout lines. `OGRE_DUMP_RDRAM=<path>` adds the 8 MiB image.

`app/src/hang_watch.cpp` writes `error.log` when the game's frame counter
`D_800AEFA4` stays still for `OGRE_HANG_MS` (default 5000; `0` is off), after a
`[hang]` line and one `[snap]` queue snapshot. The boot and a minimized window
do not count. `OGRE_HANG_MS=150` triggers it on the boot's scene loads.

Runtime message boxes and boot failures (SDL, window, renderer) also write
`error.log`.

## Releases

Releases are published on `frankjniu/ogre-battle-64-recomp-windows` from the
Windows machine:

```sh
make dist-zip DIST_OS=windows CMAKE_ARGS="-G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl"
DIST_OS=windows /usr/bin/bash tools/smoke-dist.sh        # must print PASS
gh release create vX.Y.Z dist/ogre-battle-64-recomp-windows.zip \
  --repo frankjniu/ogre-battle-64-recomp-windows --target <main commit> --draft \
  --title "vX.Y.Z (Windows)" --notes-file <notes.md>
gh release edit vX.Y.Z --repo frankjniu/ogre-battle-64-recomp-windows --draft=false
```

Bump `recomp::Version` in `app/src/main.cpp` first; mods'
`minimum_recomp_version` is checked against it. Pin `--target` to a commit,
because a draft creates its tag only when published. `gh` in a fork defaults to
the parent repository, so always pass `--repo`.

`make release` and `.github/workflows/release.yml` build on hosted runners from
a private data repository (`OGRE_DATA_REPO`) or on self-hosted runners. The fork
has neither, so its Release workflow is disabled.

## Controls and settings

Settings live in `<config>/settings.cfg` and bindings in `controls.cfg`. An
`OGRE_*` override ("Settings overrides") applies for one run and is not written
back.

### Controller bindings

| N64 | key | gamepad |
|---|---|---|
| A | `X` | A |
| B | `Z` | B |
| Z | `C` | L-Stick |
| START | `Return` | Start |
| L | `Q` | LB |
| R | `E` | RB |
| D-pad | arrows | D-pad |
| C-UP / C-DOWN | `I` / `K` | X / Y |
| C-LEFT / C-RIGHT | `J` / `L` | right stick |

The left stick is the N64 stick, and the right stick also presses the C buttons.
`ESC` opens the overlay and is never bindable. `=`/`-` and the pad's R3 change
GAME SPEED. Every pad and the keyboard feed N64 controller 1, the only one the
game reads. `controls.cfg` lines are `<n64 tag>.<key|pad> = <value>`; `-` and
`none` are unbound; key names are `SDL_GetScancodeName`'s.

On the CONTROLS tab, `SPACE` arms a capture (the next key or pad button fills
its slot), `BACKSPACE` clears the key, `DELETE` the pad, `ESC` cancels. In the
panel the pad's D-pad moves, `A`/`START` activate and `LB`/`RB` switch tabs. The
pad's SELECT opens the overlay unless a binding uses it.

### The SETTINGS rows

| row | values (saved key) | what it does |
|---|---|---|
| GAME SPEED | 1, 2, 4, 8 (`game_speed`) | scales the emulated CPU counter and VI schedule. `=`/`-` step it, R3 cycles it; a badge in the window's lower-right shows the multiplier for 2 s (not in fullscreen) |
| WIDESCREEN | off, on (`widescreen`) | 16:9 window and RT64 Expand on the mission `0x03`, cutscenes `0x0D`, world map `0x05`, Organize Screen `0x06` and HD-pack scenes; other scenes are pillarboxed. The window shape follows the toggle, never the scene |
| RESOLUTION | native, 2x, auto (`render_resolution`) | `auto` (default) is the largest integer multiple of 320x240 that fits the window's pixels |
| CRT FILTER | off, hyllian, crts, lottes (`crt_filter`) | a CRT shader in RT64's VI pass |
| MSAA | off, 2x, 4x, 8x (`msaa`) | shown and applied only with `OGRE_DISPLAY_EXPERIMENTS=1` or a set `OGRE_MSAA` |
| WINDOW | windowed, fullscreen (`window`) | RT64's fullscreen owns the window geometry; a WIDESCREEN change made in fullscreen applies on leaving it |
| SOUNDS, VOLUME | on/off, 0..100 (`sounds`, `volume`) | gain in the output callback; a mute still queues buffers, because the game is paced by the queue depth |
| MOUSE ENABLED | off, on (`mouse`) | pointer drives the world-map and mission-field cursors ("Mouse cursors") |

The window opens 1080 points tall (1440x1080; 1920x1080 with WIDESCREEN on),
and the process is per-monitor DPI aware. Every row applies on the next frame
and logs `[renderer] config: …`. 2D rectangles sample at the console's pixel
positions (RT64 `upscale2D = Original`). `docs/guides/widescreen.md` explains
how a 2D screen is widened.

The CRT filters are ports of libretro slang shaders at their default
parameters, drawn in the patched RT64 VI pass
(`VideoInterfaceCrtPS.hlsl`) from a frame box-filtered to one texel per
framebuffer pixel: `HYLLIAN` is `crt-hyllian-fast` (MIT, notice in
`packaging/README-dist.txt`), `CRTS` is `crt-lottes-fast` and `LOTTES` is
`crt-lottes` (both public domain). GPL shaders are excluded, because they would
put the executable under the GPL.

### The CHEATS tab

The rows are this ROM's (USA v1.1) GameShark list. Data rows are held once per
frame by `cheats_tick()` (`app/src/cheats.cpp`). The two battle rows patch
instructions in bank J, which is compiled C, so each is a `[[patches.hook]]` in
`config/banks/config-bankJ.toml` that reproduces the patched result while the
row is on. Turning a row off stops it and keeps the current values. A write into
a swappable bank's RAM is gated on that bank's ROM words being resident.

| row (saved key) | GameShark code | what the port does |
|---|---|---|
| MAX GOTH (`cheat_max_goth`) | `81196A8C 0098` / `81196A8E 967F` | holds goth (u32 `0x80196A8C`) at 9,999,999, the game's own cap |
| MAX ITEM COUNTS (`cheat_max_items`) | none | holds the count byte (`+0x03`) of every non-empty slot of both owned-item lists (`0x80196B20`, 278 slots; `0x80193AE0`, 40 slots) at 99 (`docs/items.md`) |
| QUICK LEVEL GAIN (`cheat_quick_level`) | `D11D639C 2C42` / `811D639E 0000` | hook before `0x801D63A0` sets `v0 = 0`, so the experience compare at `0x801D639C` always levels up |
| QUICK SOLDIER PROMOTION (`cheat_quick_promotion`) | `D11D639C 2C42` / `811D25AA 9100` | hook before `0x801D25DC` raises the leader's promotion counter (`+0x37`) to at least 100 after a battle that earned points, so one battle promotes |
| UNLOCK ALL CLASSES (`cheat_all_classes`) | `81196A96 F4FF` … `80196AA0 0001` | ORs the code's 11 bytes into the class-list bitfield at `0x80196A96` |
| DAY ALWAYS 00 (`cheat_day_zero`) | `811F108C 0000` | holds the mission day (`0x801F108C`, record 7 BSS) at 0 while record 7 is resident |
| TIME OF DAY (`cheat_hour`, `off` or 0..23) | `80196A4D 00XX` | holds the low byte of the hour (u16 `0x80196A4C`); the clock keeps running, so hour 23 still advances the date |

Each cheat logs one `[cheats]` line when it first changes the game. The USA
v1.0 codes sit 0x20 bytes lower in the army block and do not apply to this ROM.

### Quick save (F5) and quick load (F9)

The quick slot is a copy of the battery save,
`<config>/saves/<game id>-quick.bin` (`app/src/quick_save.cpp`). Both keys work
while the game window exists and the `ESC` panel is closed.

- `F5` copies the battery file (what the game last saved) to the slot.
- `F9` twice within 3 s writes the slot through the runtime's save buffer,
  waits until the battery file matches, exits, and relaunches the exe with
  `OGRE_RELAUNCH=1` (no start screen). Continue on the title reads the restored
  battery.

Messages show in the badge window and as `[quicksave]` lines.
`OGRE_QUICK_KEYS=<ms>:<f5|f9>,…` delivers scripted presses.

### The in-game overlay

`ESC` opens a borderless always-on-top SDL window over the game window
(`app/src/overlay.cpp`) with the same panel. MODS is greyed (mods load before
boot), and MAIN has EXIT GAME. While it is open the game keeps running with an
idle controller.

### Verifying the UI without a human

```sh
# every launcher tab, as a PPM
for tab in main mods controls settings cheats debug; do
  OGRE_NO_AUDIO=1 OGRE_PREF_DIR=C:/tmp/ui OGRE_LAUNCHER=1 OGRE_LAUNCHER_TAB=$tab \
    OGRE_LAUNCHER_SHOT=C:/tmp/ui-$tab.ppm "$APP"
done

# GAME SPEED 1 -> 4 in the overlay while the game runs
OGRE_NO_AUDIO=1 OGRE_PREF_DIR=C:/tmp/gs OGRE_ROM="$ROM" OGRE_OVERLAY_AT_MS=2500 \
  OGRE_OVERLAY_TAB=settings OGRE_OVERLAY_KEYS="Right,Right" \
  OGRE_CAPTURE_OVERLAY=C:/tmp/gs.ppm OGRE_OVERLAY_SHOT_MS=1200 OGRE_EXIT_AFTER_MS=7000 "$APP"
grep '^game_speed' /c/tmp/gs/settings.cfg       # game_speed = 4
```

Keys go through the real handlers, so a script's `Down` count on SETTINGS
depends on whether the MSAA row is shown.

## Mouse cursors

With MOUSE ENABLED on, a click ORs the N64 `A` bit into controller 1 for a few
pad polls, and a pointer within 24 px of a play-area bound holds the matching
D-pad bit, which scrolls the view. The OS pointer is hidden over the game window.

- **World map (`0x05`).** `app/src/map_cursor.cpp` writes `state+0x52`/`+0x54`
  (`state = *(u32*)0x80197B18`) at the cursor draw `0x801A1C48`, clamped to the
  play area.
- **Mission field (`0x03`).** `app/src/mission_cursor.cpp` writes the f32 pair
  `state+0x08`/`+0x0C` (`state = 0x800E91D0`), and follows the game's position
  while the game moves the cursor itself (D-pad, C-LEFT centring, after a click).

`tools/map_cursor.py` and `tools/mission_cursor.py` insert the calls into the
generated bank C during `make bank-recomp`.

Knobs (`MAP` and `MISSION` variants): `OGRE_MOUSE=off|on`;
`OGRE_<MAP|MISSION>_CURSOR=1|0` forces a cursor on or off; `_CURSOR_LOG=1` logs
each change; `_CURSOR_CONFIRM_POLLS=<n>` (default 8) is how long a click holds
`A`; `_CURSOR_EDGE=<px>` (default 24) and `_CURSOR_EDGE_FORCE=<mask>` set or
force the edge scroll. `OGRE_MISSION_CURSOR_YIELD=<n>` (default 15 frames) and
`OGRE_MISSION_CURSOR_AT=<x>,<y>` are mission-only. `OGRE_CURSOR=0` keeps the OS
pointer visible.

## Mods

The runtime (`librecomp/src/mods.cpp`) loads every `.nrm`, recompiles a code
mod's MIPS at load time, applies its hooks and replacements, and stores toggles
and options. `MOD_GAME_ID` is `ogrebattle64` (`app/src/game.hpp`).

| path | what |
|---|---|
| `<config>/mods/*.nrm` | one file per mod |
| `<config>/mod_config/<mod id>.json` | that mod's option values |
| `<config>/mods.json` | enabled mods and their order |

The MODS tab of the start screen lists one row per mod and one per option.
Deleting a `.nrm` removes a mod. `OGRE_MOD_TEST=1` starts a run with
`mods.json` as it stands and no start screen.

### Building an example mod

```sh
# WSL
make mod-syms        # -> mods/reference/{dump,data_dump}.toml
make example-mods    # -> build/mods/*.nrm
```

`make mod-syms` dumps `build/ogrebattle64.elf`'s context; the mod tool resolves
hooked names against it. `tools/build-example-mods.sh` compiles each mod's
`src/*.c` with `mips-linux-gnu-gcc` and runs `RecompModTool`. Copy
`build/mods/*.nrm` to the Windows checkout. `mods/skip-boot-logos/` is the
smallest example: one entry hook on scene `0x09`'s update. Example mods ship
with `enabled_by_default = false` (a field `patches/n64recomp-ob64.patch` adds).

### What a hook can reach

A hook resolves through `get_vrom_to_section_map()`, which holds the base ELF's
sections only: `.entry`, `.main`, `.streamedA/B/C`. A function in a streamed bank
unit cannot be hooked; a mod that needs bank behaviour hooks the per-frame
function `func_80072944` and works on RAM. The port's own code inside bank
functions goes in at build time, through `[[patches.hook]]` entries in
`config/banks/config-bank<U>.toml` or the generated-C patch scripts
(`tools/map_cursor.py`, `tools/mission_cursor.py`).

Hooking a bank function would take three changes. Add the bank record's section
and functions to the reference dump that `func_reference_syms_file` names (merge
each unit's `--dump-context` output in `make mod-syms`). Give the runtime's
`build_regen_list` (`librecomp/src/mods.cpp`) a way to resolve a hook's ROM
address to a bank record's RAM base and function table. Register the 44 bank
records from `app/src/bank_funcs.inc` with that resolver at startup
(`register_bank_overlays()`). No relocations are needed, because bank code
already holds linked absolute addresses and every call in a regenerated function
becomes a `get_function` lookup.

## Saves

### The cartridge battery (SRAM)

OB64 saves to 32 KiB of battery SRAM, written to
`<config>/saves/ogrebattle64-us-rev1.bin`. The Controller Pak is only the game's
copy/backup device. Slot `n` is `0x1850` bytes at device offset
`0x10 + n*0x1850`, signed `QuestOG3` at `+0x04`; only slots 0 and 1 hold games.
Offset `0x30B0` is the game's 19176-byte map record. A slot's first two `u16`
are checksums over `slot+0x0C .. slot+0x1850` (byte sum and set-bit count), each
plus the slot's device offset, so a slot is valid only at its own index. The
save field layout is in `docs/items.md`.

The runtime prints `[save]` lines on load, first read, first write and each
write. A failed write raises a message box and stalls the run; use a writable
`OGRE_PREF_DIR`.

### Importing emulator saves

```sh
tools/sramsave.py check  save.srm                      # what is in the file; ok / MISMATCH per slot
tools/sramsave.py import save.srm <config>/saves/ogrebattle64-us-rev1.bin
tools/sramsave.py export <that .bin> out.sra           # 32-bit byteswapped
tools/sramsave.py pak --all assets/saves/prologue.n64  # list a pak's notes
```

`import` finds the SRAM by its magic at any offset and byte order (mupen64plus
`.srm`, parallel-n64). A DexDrive `.N64` file is a Controller Pak whose game
notes each hold a battery slot; `import` reseeds the live notes into battery
slots 0 and 1 (`--all` also takes deleted notes).

### Running with a save — `OGRE_SAVE`

```sh
OGRE_NO_AUDIO=1 OGRE_SAVE=prologue OGRE_SCENE=title "$APP" "$ROM"
OGRE_NO_AUDIO=1 OGRE_SAVE=C:/saves/save.srm OGRE_SAVE_RESET=1 "$APP" "$ROM"
```

`OGRE_SAVE=<name|path>` installs that save as the battery; a bare name is tried
as a path, then `<rom dir>/saves/<name>[.n64|.bin]`, then
`assets/saves/<name>[.n64|.bin]`. The source is never written.
`OGRE_SAVE_RESET=1` re-imports even when the config dir's battery is newer;
`OGRE_SAVE_ALL=1` also takes deleted notes.

`tools/run-save.sh <save> [rom]` gives each save its own config directory
(`.ogre-prefs-save-<name>/`, gitignored) with `--reset`, `--list` and
`--import-only`. It runs `OGRE_APP` or the first built exe it finds.

The Load Game book (scene `0x12`) shows which battery the game read. A blank
battery also reaches `0x12` and `0x05`, so the route alone proves nothing.

## Environment knobs

All knobs default to off.

### Run control and scripted input

| variable | effect |
|---|---|
| `OGRE_NO_AUDIO=1` | open no audio device; the audio microcode still runs. Set on every test run |
| `OGRE_EXIT_AFTER_MS=<n>` | after `n` ms print each game thread's last function and call chain, then `_Exit(0)` |
| `OGRE_PREF_DIR=<dir>` | config directory |
| `OGRE_ROM=<path>`, `OGRE_LAUNCHER=1`, `OGRE_MOD_TEST=1` | ROM without an argument; force the start screen; skip it with mods installed |
| `OGRE_TEST_DROP=<path>` | one synthetic ROM drop on the start screen |
| `OGRE_SCENE=<name\|hex>`, `OGRE_SCENE_AFTER_MS=<n>` | boot into a scene ("Scene forcing"); `OGRE_FORCE_SCENE` is the old hex-only spelling |
| `OGRE_STEP=<n>` | seed the New Game step word `D_8018F1C0` once when the target scene is active. It changes the sequence's state; drive the title with taps for a real New Game |
| `OGRE_SMOKE=1` | print `[smoke] main reached`, try SDL once, exit 0 |
| `OGRE_CRASH_TEST=<segv\|bus\|abrt\|fpe\|ill>` | raise that signal after the crash logger is armed; the argument `--crash-test=<name>` does the same |
| `OGRE_CONSOLE=1`, `OGRE_LIVE_CONSOLE=1` | allocate a console; the live console ("The live console") |
| `OGRE_TAP_MS=<n>` | controller 1 presses Start (or `OGRE_TAP_BUTTON`) for 150 ms every `n` ms; `OGRE_TAP_MAX=<n>` stops after tap `n` |
| `OGRE_TAP_BUTTON=<list>` | one button per tap (`start`, `a`, `b`, `down`, …; `none` skips). On the name form (`0x07`) use a few `start,a` pairs then a long run of `a` |
| `OGRE_TAP_SCENE` / `OGRE_TAP_NOT_SCENE=<list>` | tap only while / except while `D_800E810E` is in the list |
| `OGRE_TAP_SCENE_BUTTON=<scene>:<button>[:<count>],…` | per-scene presses, e.g. `"title:start:5,0x12:a:3,0x05:right:3,0x05:a:1"` |
| `OGRE_LAUNCHER_TAB` / `_KEYS` / `_PAD` / `_SHOT` / `_SHOT_MS` | start-screen tab; synthetic keys (SDL scancode names) or pad presses every 150 ms; write a PPM and quit |
| `OGRE_OVERLAY=1`, `OGRE_OVERLAY_AT_MS=<n>` | open the overlay at startup / push one `ESC` after `n` ms |
| `OGRE_OVERLAY_TAB` / `_KEYS` / `_PAD` / `_OPACITY` | overlay tab; keys while visible; pad presses (a first `back` opens it); opacity 0.2..1.0 |
| `OGRE_CAPTURE_OVERLAY=<path>`, `OGRE_OVERLAY_SHOT_MS=<n>` | the overlay as a PPM once |
| `OGRE_WINDOW_CLOSE_AT_MS=<n>` | one close event for the game window |
| `OGRE_SPEED_KEYS=<up\|down>,…`, `OGRE_SPEED_PAD=<tag>,…`, `_AT_MS` | synthetic `=`/`-` or R3 presses |
| `OGRE_CAPTURE_BADGE=<path>` | the speed badge as a PPM on every show |
| `OGRE_QUICK_KEYS=<ms>:<f5\|f9>,…` | scripted quick save/load presses |

### Settings overrides

| variable | effect |
|---|---|
| `OGRE_SPEED=<n>` | GAME SPEED, 1..64 |
| `OGRE_WIDESCREEN=off\|on` | WIDESCREEN |
| `OGRE_WS_SCENES=<hex>[,<hex>…]` | the Expand scene set (default `0x03,0x0D,0x05,0x06` plus HD-pack scenes) |
| `OGRE_RESOLUTION=native\|2x\|auto` | RESOLUTION |
| `OGRE_CRT=off\|hyllian\|crts\|lottes` | CRT FILTER |
| `OGRE_UPSCALE_2D=original\|scaled\|all` | RT64's `upscale2D` |
| `OGRE_DISPLAY_EXPERIMENTS=1`, `OGRE_MSAA=off\|2x\|4x\|8x` | show and apply MSAA |
| `OGRE_WINDOW=windowed\|fullscreen` | WINDOW |
| `OGRE_SOUNDS=off\|on`, `OGRE_VOLUME=<0..100>` | SOUNDS, VOLUME |
| `OGRE_MOUSE=off\|on` | MOUSE ENABLED |
| `OGRE_CHEAT_MAX_GOTH`, `_MAX_ITEMS`, `_QUICK_LEVEL`, `_QUICK_PROMOTION`, `_ALL_CLASSES`, `_DAY_ZERO` (`off\|on`); `OGRE_CHEAT_HOUR=off\|0..23` | the CHEATS rows |
| `OGRE_GRAPHICS_API=auto\|d3d12\|vulkan` | pin RT64's backend. RT64 falls back from D3D12 to Vulkan on old drivers; the boot log names the GPU and the choice |

### Logs, traces and dumps

| variable | effect |
|---|---|
| `OGRE_SCENE_LOG=1` | one `[scene]` line per scene change, with descriptor and record mask |
| `OGRE_SCENE_TRACE=1` | the scene-forcing state every 500 ms |
| `OGRE_RSP_LOG=1` | `[rsp]` lines per RSP task; `tools/runlog.py` needs them |
| `OGRE_DL_TRACE=1` | one line per display list with its ucode and RT64's time |
| `OGRE_DEBUG_TRACES=1` | `[ev]`/`[mq]`/`[sch]`/`[vi-debug]` and `[pi] inline DMA`; `OGRE_DEBUG_VI=1` adds VI mode changes |
| `OGRE_VI_COUNT=1` | VI retrace messages per second (half of it is the frame-rate ceiling) |
| `OGRE_HANG_MS=<ms>` | hang watch threshold (default 5000; `0` off) |
| `OGRE_LOOKUP_CHECK=0` | turn off the lookup check ("The lookup check") |
| `OGRE_SNAP=1\|<frames>` | periodic `[snap]` message-queue snapshot (default every 90 VI frames); each one stalls the VI about 1.6 s on Windows |
| `OGRE_CHAIN_HISTORY=<tid>` | the call chains thread `<tid>` goes through |
| `OGRE_SCHED_TRACE=1`, `_TID=<tid>` | scheduler traces and the `[sched-ring]` ring at exit |
| `OGRE_PROFILE=1`, `OGRE_PROFILE_CHAINS=1` | sampling profiler and shadow call chain (the per-thread function is the most recently entered one); also print threads' chains |
| `OGRE_TRACE_HOOKS=1` | turn on the function entry/return trace hooks without another consumer; slows the boot several-fold |
| `OGRE_COVER=1\|<path>` | at exit, one `[cover] 0xADDR count` line per function entered |
| `OGRE_DMA_TRACE=1`, `_FULL=1` | at `OGRE_EXIT_AFTER_MS`, every streamed PI DMA by `(rom, ram)`; changes timing |
| `OGRE_PROBE55=1` / `OGRE_PROBE57=1` | New Game sequence state on change (scene, `D_8018F1C0`, `D_8018F1C2`, plus script or engine words) |
| `OGRE_DUMP_RDRAM=<path>` | the 8 MiB RDRAM image (host layout) at exit and on a crash |
| `OGRE_RAM_KEY=<1..9>` | that key writes `ram-<n>.bin`/`.txt` to `OGRE_RAM_KEY_OUT` (else `OGRE_PREF_DIR`, else `/tmp`). `_ADDR`, `_LEN`, `_STRIDE`, `_AT_MS` (timed dump), `_FULL=1` (whole image, ROM order), `_ON_BLACK=1` (whole image at the first black draw). `tools/ogre_ram.py` reads it |
| `OGRE_AUDIO_STATS=1` | one `[audio]` line per second: buffers pushed, device pulls, underruns, FIFO depth |
| `OGRE_AUDIO_REFILL_MS=<n>` | refill threshold after an underrun (default 40, at most 48) |
| `OGRE_AUDIO_SINK=<path>` | with `OGRE_NO_AUDIO=1`, drain the FIFO on a real-time clock into raw S16LE stereo at 32000 Hz, so a silent run can measure underruns |
| `OGRE_AUDIO_MARK_KEY=<1..9>` | that digit prints `[audio] mark <n>` |
| `OGRE_AUDIO_RESAMPLE=app` | open the device at its own rate and resample in the callback (default: open at 32000 Hz and let Windows convert) |

### Renderer diagnostics

| variable | effect |
|---|---|
| `OGRE_DL_ANALYZE=1`, `OGRE_DL_DECODE=<n>\|all`, `OGRE_DUMP_DL=1` | per-list counts / decoded commands / hexdump of each list's first words. All decode as F3DEX2, so an S2DEX2 list reads wrong |
| `OGRE_TEX_TRACE=1` | one `[tex]` line per `G_TEXRECT`: rectangle, `s`/`t`/`dsdx`/`dtdy`, tile, palette, `SETTIMG` |
| `OGRE_TEX_TRACE_KEY=<1..9>`, `_KEY2`, `_MS`, `_AFTER_MS` | arm the trace on a key, close it after `_MS` (default 2000); a second key; a timer |
| `OGRE_TEX_TRACE_DUMP=<dir>` | also write the sampled RDRAM; `tools/ogre_tex_uv.py <dir>` summarises |
| `OGRE_TEX_TRACE_COLOUR=1`, `OGRE_TEX_TRACE_BLACK=<n>` | decode each window's mean colour; flag the first draw at or below `n`/255 |
| `OGRE_TEX_TRACE_DRAW`, `_MAX`, `_MIN_PX`, `_IMAGES` | only the nth rectangle / a cap / skip small rectangles / each image once |
| `OGRE_PRESENT_TRACE=1` | `[present]`/`[state]` per present |
| `OGRE_RDP_TRACE=1\|2` | `[rdp]` colour-image and fill/rect calls; `2` adds a colour-image histogram |
| `OGRE_WORKLOAD_TRACE=1`, `OGRE_FBRENDER_TRACE=1`, `OGRE_VI_TRACE=1` | framebuffer pairs per workload; per-call render state; the decoded VI |
| `OGRE_RECT_STATE=<y0>-<y1>` | render state of rectangles inside those rows |
| `OGRE_TILE_TRACE=1`, `OGRE_TILE_TRACE_ALL=1` | `[gpu-tile]` for degenerate tiles / every tile (`_MAX`, `_MAXH`, `_MISS`, `_COPY`) |
| `OGRE_SPRITE_TRACE=1` | `[spr]` per tile of a two-texture draw: upload path, hash, tile copy |
| `OGRE_TILE_DECODE=1` | RGBA decoded from each hashed tile's TMEM; `[fbcopy*]` lines for framebuffer tile copies and refusals |
| `OGRE_DUMP_TEX=<dir>` | RT64's decoded-texture dump (`.tmem` + `.tile.json`) |
| `OGRE_EMPTY_TILE_TRACE=1` | rectangles skipped because their tile covers no texels |
| `OGRE_S2D_TRACE=1` | the first 40 S2DEX object commands |
| `OGRE_CONVERT_TRACE=1` | the raw `G_SETCONVERT` fields |
| `OGRE_FB_WRITEBACK=1` | framebuffer pairs and RDP→RDRAM writeback |
| `OGRE_SYNC_TRACE=1` | the framebuffer manager at the njpeg CPU readback |
| `OGRE_YUV_TRACE=1` | each `G_SETTIMG` with `fmt=YUV` |

### Renderer behaviour

| variable | effect |
|---|---|
| `OGRE_RECT_LR_FIX=1` | restore RT64's "Fix LR with Scissor" (off; it widened the Organize Screen's font rectangles) |
| `OGRE_OVERSCAN=<0..8>` | crop the framebuffer's right and bottom edge (default 1 px; the game's scissor leaves the last row and column stale). The wide world map turns it off |
| `OGRE_FB_GPU_COPY=0` | turn off RT64's GPU framebuffer copies; every load reads RDRAM |
| `OGRE_NOP_RECT=<selectors>` | replace matching `G_TEXRECT`s with no-ops (`any`, `tex:<hex>`, `flip`, `x:<a>-<b>`, `y:<a>-<b>`, `n:<index>`) |
| `OGRE_EMPTY_TILE=draw` | draw rectangles whose tile covers zero texels |
| `OGRE_FOG=0\|alternate`, `OGRE_FOG_SCALE=<percent>`, `OGRE_FOG_TRACE=1`, `OGRE_FOG_SUMMARY=1` | the title-fog repair (layer table `D_801B80E0`), its intensity, its traces |
| `OGRE_PRESENT_ALWAYS=1`, `OGRE_PRESENT_FBTARGET=1`, `OGRE_INSTANT_PRESENT=1` | present every VI; present a non-empty render target when no framebuffer is at the VI address; RT64 `PresentEarly` |
| `OGRE_PRESENT_SCRATCH=1` | always present the RAM copy of the VI framebuffer |
| `OGRE_PRESENT_SIGNAL=1`, `_CYCLE`, `OGRE_PRESENT_MAGENTA=1` | replace the VI draw with a flat colour: green = RAM copy has content, red = empty, blue = no colour target |

### Captures and the synthetic probe

| variable | effect |
|---|---|
| `OGRE_CAPTURE_PRESENT=<path>` | the swap-chain texture at each present as `<path>.<n>.ppm` |
| `OGRE_CAPTURE_TARGET=<path>` | the render target the VI pass sampled as `<path>.<n>.bin`. It currently crashes at the first capture (PLAN.md) |
| `OGRE_CAPTURE_AFTER=<n>`, `OGRE_CAPTURE_EVERY=<n>` | skip the first `n` presents / capture every `n`th |
| `OGRE_SYNTH_FRAME=1` | submit a seven-bar F3DEX2 list through the task path |
| `OGRE_SYNTH_AT_MS`, `_PERIOD`, `_RAW`, `_ANIMATE`, `_NO_DL`, `_FORCE_VI=0` | first submit (above ~600 ms), VIs between submits, and variants |
| `OGRE_NO_DUMMY_VI=1` | skip the pre-start dummy VI workload |

### njpeg and HD backgrounds

| variable | effect |
|---|---|
| `OGRE_NJPEG=0`, `OGRE_AUDIO_UCODE=0` | stub the njpeg decoder / the audio microcode |
| `OGRE_NJPEG_TRACE=1` | the colour image the YUV draw landed in |
| `OGRE_NJREAD_LOG=1` | one `[njread]` line per readback pass (`docs/guides/njpeg-backgrounds.md`) |
| `OGRE_BG=0`, `OGRE_BG_DIR=<dir>` | run without the `mods/backgrounds/` pack / use only that folder |
| `OGRE_BG_LOG=1`, `OGRE_BG_DEBUG=1` | `[hdbg]` per pass and chunk, with each njpeg pass's asset key / paint the passes in solid colours |
| `OGRE_BG_DUMP=<dir>` | write the game's 496x384 backdrop as `original-step<NNNN>.png`, and each njpeg asset once as `njpeg-<key>-<w>x<h>.png` |
| `OGRE_BG_HD=0` | skip the HD strips (`mods/backgrounds/hd/`) |

`docs/guides/hd-backgrounds.md` covers the pack.

## Scripted runs and captures

Scripted taps are for short, exact routes. A played state comes from a save
(`OGRE_SAVE`; AGENTS.md rules 11 and 13).

### Scene forcing

`OGRE_SCENE` names: `title` (0x04), `intro` (0x09), `publishers` (0x0A),
`story` (0x0B), `unit-info` (0x0C), `tutorial` (0x17), `menu` (0x18),
`new-game` (0x02), or a hex id. The app writes the game's own request,
`0x8000 | id`, into `D_800C4C26` until `D_800E810E` reports the target, so the
current scene leaves as in a natural transition. A forced scene lacks the
pre-state a natural path builds; forced `menu` and `new-game` crash.

```sh
OGRE_NO_AUDIO=1 OGRE_SCENE=title OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=6000 "$APP" "$ROM"
OGRE_NO_AUDIO=1 OGRE_SCENE=title OGRE_TAP_MS=2500 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,a,a,a,a,a,a,a" "$APP" "$ROM"   # New Game
```

### Capturing the presented frame

A desktop screenshot can return a stale frame; `OGRE_CAPTURE_PRESENT` reads the
swap-chain texture.

```sh
OGRE_NO_AUDIO=1 OGRE_SCENE=title OGRE_SPEED=4 OGRE_CAPTURE_PRESENT=C:/tmp/title \
  OGRE_CAPTURE_AFTER=1700 OGRE_CAPTURE_EVERY=10 OGRE_EXIT_AFTER_MS=26000 "$APP" "$ROM"
```

An environment value wins over the console's `snap`. Without
`OGRE_PRESENT_ALWAYS` a stalled boot keeps an old frame.

## Diagnostics toolkit

### Reading a dump

The runtime stores RDRAM with each 32-bit word byte-reversed: the guest word at
`a` is the little-endian word at host offset `a & 0x1FFFFFFF`, and the guest byte
is at `(a & 0x1FFFFFFF) ^ 3`. `OGRE_DUMP_RDRAM` and the console's `dump`/`snap`
write that host layout; `OGRE_RAM_KEY_FULL=1` writes ROM order.
`rdram.load_image` (used by `rdram.py`, `ogre_ram.py`, `ogre_frame.py`,
`refscan.py`, `scenemap.py --dump`) detects the layout and converts to ROM order,
so every tool reads the guest word at `a` as the big-endian bytes at
`a & 0x1FFFFFFF`.

```sh
tools/rdram.py <dump> word|half|byte|string|hexdump|find|ptr …
tools/rdram.py <dump> banks        # which module is resident in each streamed window
tools/rdram.py <dump> image 0x80243E28 --width 320 -o C:/tmp/bg.png   # rgba16, rgba32, ia16, ia8, ia4, i8, i4, ci8
tools/rdram.py diff C:/tmp/a.bin C:/tmp/b.bin --min-run 16 --limit 20
tools/ogre_frame.py <dump> --log C:/tmp/run.log --out C:/tmp/frames   # render the logged DL roots offline
```

`banks` shows a wrong-bank call as another module's bytes at the faulting
address. `image` flags a `(near-)uniform` region ("nothing drew this" against
"an asset failed to decode"). `diff` groups changed runs by owning record.

### The live console

The console runs commands in the running game on the main thread. It is off
unless asked for, because `w` writes guest RAM. `OGRE_LIVE_CONSOLE=1` turns on
both triggers: a watched file (`OGRE_CONSOLE_FILE`, default
`/tmp/ogre-console.txt`; its lines run and it is removed; `OGRE_CONSOLE_AT_MS`
delays reads) and keys `1`..`9` (`OGRE_KEY_<n>=<command>`). Write the file
atomically. Output is `[console]` lines on stdout.

```sh
OGRE_NO_AUDIO=1 OGRE_LIVE_CONSOLE=1 OGRE_CONSOLE_FILE=C:/tmp/ogre-console.txt \
  "$APP" "$ROM" | tee C:/tmp/live.log
printf 'c\nr 0x8018FDAC 8\ndump C:/tmp/at-now.bin\n' > C:/tmp/oc.tmp \
  && mv C:/tmp/oc.tmp C:/tmp/ogre-console.txt
```

| command | what it does |
|---|---|
| `r` / `rh` / `rb` / `rk <addr> [n]` | words / halfwords / bytes / raw host-order words |
| `d <addr> [len]`, `s <addr> [len] [max]` | hex and ASCII dump; strings in a range |
| `f <value> [max]`, `fb <hexbytes> [max]` | find a word / a byte pattern |
| `k <addr> [len]` | FNV-1a checksum |
| `w <addr> <value>` | store a word |
| `c` | scene, pending scene, descriptor, record mask, step |
| `dump [path]`, `snap [prefix] [ms]` | the image now / with game threads parked, plus frame captures for `ms` |
| `save [path]`, `load [path]` | write / restore a checkpoint |
| `press <buttons> [polls] [x] [y]` | hold a pad press (`+`-joined tap names) for `polls` input polls (hex) |
| `cover`, `help` | functions entered so far (needs `OGRE_COVER`); the list |

`OGRE_CONSOLE_ON_SCENE=<name|hex>`, `OGRE_CONSOLE_ON_STEP=<n>` and
`OGRE_CONSOLE_ON_CMD=<cmd>` run one command on the first frame that scene (and
step) is active, for example `save C:/tmp/ck.ckpt`.

A checkpoint is RDRAM plus the loaded bank records, written with the game
threads parked. It is valid only in the process and binary that wrote it, and
does not rewind knob or RT64 state. Save inside a stable step (check `c`
first); a checkpoint taken at a `0x02` loader visit restores into the wrong
branch.

### Run-log tools

```sh
OGRE_NO_AUDIO=1 OGRE_SPEED=4 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D OGRE_SCENE_LOG=1 \
  OGRE_RSP_LOG=1 OGRE_DL_TRACE=1 OGRE_EXIT_AFTER_MS=45000 "$APP" "$ROM" > C:/tmp/run.log 2>&1
tools/runlog.py C:/tmp/run.log [--check] [--json]   # scene timeline, RSP tasks, bank loads, problems
tools/dlgaps.py C:/tmp/run.log [--check]            # frame-gap distribution; --check fails on periodic spikes
tools/proflog.py C:/tmp/run.log                     # an OGRE_PROFILE=1 log: slow phases by function
```

`runlog.py --check` exits 1 on a crash, a stub call, an unknown module or a log
with no task lines. A non-gfx task served by `stub microcode` is work the port
drops. A periodic stall in `dlgaps.py` is the signature of the `[snap]`
snapshot or of a poll-loop `yield_self` in a work loop.

### A slow screen

`OGRE_PROFILE=1`'s `t4` line names the most recently entered function, which is
a lead. A debugger stack sample of the game threads names the function each is
in; a chain through `yield_self` means the frame period is a recompiler-injected
poll-loop wait. The usual cause is a work loop that N64Recomp's
`is_yield_poll_loop` took for a poll (for example, a list walk that compares
each node with a base the loop never writes). Fix it with an entry in the unit's
`yield_work_loop_branches` (`config/banks/config-bank<U>.toml`).

### Offline ROM and ELF tools (WSL)

```sh
tools/guestmap.py 0x801B7EBC [--context 4]   # rom<->vram, owning record and unit, entry check, RAM sharers
tools/scenemap.py [scenes|transitions|steps] # 25 scene descriptors, 39 pending-scene writers, 1693 steps
tools/scenemap.py scenes --dump C:/tmp/map3.bin   # descriptors from a live image
tools/arenamap.py [--missing|--verify|--coverage|--arena 0x80214FA0]   # every streamed bank load
tools/stubmap.py report | target <addr> | log <run.log> | pointers      # cross-bank call targets
tools/recompcov.py [--log cover.txt]         # static and executed coverage
make elf-rom-check                           # every linked ELF against the ROM
make cross-bank-check                        # direct calls into another section's swappable RAM
```

- `arenamap.py` reads each bank load (`jal func_8009DA50` plus its cache and BSS
  calls) for ROM start, RAM base, size and the code/data split. A bank's size is
  the loader's `subu`. Table-driven loads (records 16 and 18) have no literal
  loader and need a DMA census (`OGRE_DMA_TRACE=1 OGRE_DMA_TRACE_FULL=1`).
- `stubmap.py` buckets: DEFINITE (no module registers the address), MISSING
  FUNCTION (a resident-capable bank lacks the entry; act on these first),
  UNRESOLVABLE, BANK SELECTION, INERT, RSP IMEM. `log` names the module resident
  at each stub hit.
- `elfcheck.py` also asserts every symbol with eight hex digits in its name is at
  that address. Run it when a scene reads the wrong entry of a table: `as` pads
  `.text` to 16 bytes, so an 8-byte-aligned code/data boundary links data 8
  bytes high. Fix it in the unit's config.
- For execution coverage, play with `OGRE_COVER=C:/tmp/cover.txt`, close the
  window, and pass the file to `recompcov.py`.

### The lookup check — `[lookup]` lines

`get_function` asks `lookup_check` (`app/src/bank_overlays.cpp`) about every
address in `0x8016A000..0x80400000`, which covers every `LOOKUP_FUNC` and every
indirect `jalr` into streamed RAM. It compares the resident words with the
mapped body's ROM words up to its first `jr ra`, and on a mismatch or a miss
calls the body whose ROM words are resident:

```
[lookup] 0x8019A884: the map has the body at rom 0x…; the resident code is rom 0x…, so that body runs
[lookup] 0x8019A884: the resident code matches no compiled body; the map's body (rom 0x…) runs
```

The first form (also `not in the map`) is a bank load `on_streamed_dma` did not
recognise; the second is a module the port has not compiled. Direct calls bypass
the check. `OGRE_LOOKUP_CHECK=0` turns it off.

### Adding a streamed bank unit

1. Read the port's report: `[bank] UNKNOWN module rom=0x… ram=0x…`,
   `[overlays] streamed function stub called @ 0x…`, or a `[lookup]` line.
2. Add `config/banks/config-bank<U>.yaml` (splat: `type: code`, `start`/`vram`,
   `[start, asm]` + `[<code end>, data]`, `symbol_name_format: "ovl<U>_$VRAM"`),
   `config/banks/config-bank<U>.toml`, and `BANK_UNITS += <U>` in the
   `Makefile`. The size is the DMA size; the code/data boundary is the last
   `jr ra` rounded up to 16 bytes.
3. If the segment table's `ram_end` is past `ram start + rom size`, add the
   record to `RAM_END` in `tools/gen_bank_funcs.py` so its BSS is zeroed on load.
4. Declare every address the game calls that the record never `jal`s internally
   in `config/symbols/symbol_addrs-bank<U>.txt` (`name = 0xADDR; //
   type:func`). Otherwise it is a label inside a body and `LOOKUP_FUNC` resolves
   to the stub. Derive the list from the `LOOKUP_FUNC(0x…)` targets in
   `Bank<U>Funcs/` inside the unit's RAM that are not a `recomp_trace_entry`.
5. If a call runs the wrong bank's body, move the target record to a unit that
   does not call it, so the caller compiles as `LOOKUP_FUNC`. A `jal` from the
   main ELF into swappable RAM goes on the `cross_bank.py dispatch --only` list
   in the `Makefile`. `tools/cross_bank_known_hazards.txt` lists accepted
   hazards, and a new one fails `make bank-recomp`.

Then regenerate in WSL and copy the outputs ("Windows setup" step 3).

### Watching a guest address

Set a debugger data breakpoint on the host address
`rdram_base + (guest - 0x80000000)`; `error.log` prints the RDRAM base
(`OGRE_CRASH_TEST=segv` writes one on demand). `tools/watch.sh` does the same
under lldb and has been used on macOS only.

## Troubleshooting

The build and shell traps are in "Windows setup" step 6. Also:

- **RT64 submodules do not check out** ("Unable to find current revision"):
  `git -C tools/RT64/src/contrib/imgui fetch origin <sha>`, then `checkout <sha>`.
- **`cl : command line error D8021`**: `build-dist/` was configured without the
  clang-cl `CMAKE_ARGS`. Delete it and rerun `make app` with them.
- **"the recompiled game code is missing"**: copy the generated code (setup
  step 3).
- **RT64 build fails after a toolchain or `tools/RT64/CMakeLists.txt` change**:
  delete `build-dist/` and rebuild.
- **A console command never runs**: the file went to Git Bash's `/tmp`
  (`%TEMP%`) while the app polls `C:\tmp\`, or it was written non-atomically.
- **The game crashes at start**: update the graphics driver, then try
  `OGRE_GRAPHICS_API=vulkan` or `d3d12`.
