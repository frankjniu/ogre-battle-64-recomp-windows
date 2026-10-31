# Building, running and debugging the Windows port

The app (`app/`) compiles the recompiled game code against N64ModernRuntime,
links the RT64 renderer, and boots the game from the player's ROM. This guide
covers the Windows build. The generated game code is produced in WSL, because
Windows has no MIPS toolchain, and the app is built natively with clang-cl.

Commands are Git Bash unless a block says PowerShell or WSL. The examples use
two shell variables:

```sh
APP=build-dist/ogrebattle64.exe
ROM=/c/path/to/ogre64.z64
```

Every game run sets `OGRE_NO_AUDIO=1` (AGENTS.md rule 14), and every example
line below carries it.

## Windows setup

### 1. Compiler, CMake, Ninja and make

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --add Microsoft.VisualStudio.Component.VC.CMake.Project"
choco install make
```

The developer's machine has MSVC 14.44.35207, Windows SDK 10.0.26100.0,
clang-cl 19.1.5, and the CMake 3.31.6-msvc6 and Ninja that ship with the Build
Tools. `make` comes from Chocolatey. Git Bash (Git for Windows) is the shell.

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

The Makefile expects the binaries at `tools/N64Recomp/build/N64Recomp`,
`RSPRecomp` and `RecompModTool`. `make example-mods` writes `build/mods/*.nrm`,
which `make dist` requires.

The WSL tree also holds everything that needs the ROM or the MIPS ELFs: `asm/`,
`build/*.elf`, `assets/`, `mips-linux-gnu-objdump`. The Windows checkout has
none of these, so the offline tools that read the ROM or `build/*.elf`
(`scenemap.py`, `arenamap.py`, `guestmap.py`, `stubmap.py`, `recompcov.py`,
`elfcheck.py`, `cross_bank.py`) run in the WSL tree.

### 3. Copy the generated code into the Windows checkout

```sh
# WSL, from ~/ob64
W=/mnt/c/Users/<you>/Development/ogre-battle-64-recomp
rm -rf "$W"/RecompiledFuncs "$W"/Bank*Funcs "$W"/RspFuncs
cp -r RecompiledFuncs Bank*Funcs RspFuncs "$W"/
cp app/src/bank_funcs.inc "$W"/app/src/
mkdir -p "$W"/build/mods && cp build/mods/*.nrm "$W"/build/mods/
```

All of these are gitignored. The `rm -rf` removes a unit directory that a
regeneration no longer produces.

Regenerate and copy again whenever `config/`, `asm/` or the Makefile's dispatch
list (`cross_bank.py dispatch --only …`) changes: run `make regenerate`, or
`make bank-recomp && make recomp` in that order when only bank configs or the
dispatch list changed. Keep the WSL tree's `Makefile` and `config/` in sync
with the Windows checkout. A WSL clone carries only committed state, so copy
uncommitted edits across before regenerating.

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

`--ignore-whitespace` is needed because a Windows checkout gives the RT64 tree
CRLF line endings, and `git apply` then fails on a hunk whose last line has no
newline (`patches/rt64-ob64.patch` has two). See "Third-party patches".

### 5. Build, package, smoke-test

```sh
make app DIST_OS=windows CMAKE_ARGS="-G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl"
make dist DIST_OS=windows
DIST_OS=windows /usr/bin/bash tools/smoke-dist.sh
```

`make app` builds a static SDL2 first (`make sdl2-static`) and then the app in
`build-dist/`; the executable is `build-dist/ogrebattle64.exe`. `make dist`
reuses the configured `build-dist/` and writes `dist/ogre-battle-64-recomp/`.
Run `make app` with `CMAKE_ARGS` before the first `make dist`: a `build-dist/`
configured without them uses the default generator and `cl.exe`, which fails.

### 6. Traps on this machine

- **`VsDevCmd.bat` fails on a long `PATH`** with `'vswhere.exe' is not
  recognized` or `\Microsoft was unexpected at this time`. Capture its
  environment from PowerShell with a minimal `PATH`:

  ```powershell
  $env:PATH = 'C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem;C:\Program Files (x86)\Microsoft Visual Studio\Installer'
  cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 && set' > vsenv.txt
  ```

  Import the `NAME=value` lines into Git Bash (convert `PATH` with
  `cygpath -p`), and prepend these Build Tools directories to `PATH`:
  `VC\Tools\Llvm\x64\bin` (clang-cl),
  `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` and
  `Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja`.
- **`bash` resolves to WSL** once the VS `PATH` is first: the name finds
  `System32\bash.exe`. Run repo scripts with `/usr/bin/bash`.
- **`python3` may be the Microsoft Store stub.** It breaks
  `tools/smoke-dist.sh`'s `tools/pe_imports.py` call. Put a real `python3` first
  on `PATH`.
- **`build-dist/ogrebattle64.exe` does not start in place** until
  `dxcompiler.dll` and `dxil.dll` are copied beside it from `build-dist/RT64/`.
  `make dist` copies them into the package.
- **`wsl -d Ubuntu -- bash -c '...'` expands `$VAR` and `$(...)` on the Windows
  side**, because the line passes through the default shell first. Use
  `wsl -d Ubuntu --exec bash -c '...'` or a script file.

### 7. Running from a shell, and Defender

`"$APP" "$ROM" > run.log 2>&1` keeps its log: a redirected stream stays on its
file or pipe. A double-clicked launch has no standard handles; its log still
reaches `error.log` on a crash.

To test a launch with no standard handles, start the exe through
`explorer.exe <exe>`, or through `explorer.exe <shortcut.lnk>` to pass
arguments; `tools/smoke-dist.sh` check 5 does the latter with
`--crash-test=segv`. Explorer starts the program with its own environment, so
an `OGRE_*` variable set in the calling shell does not reach it. Scripts that
cleared the handles and used `DETACHED_PROCESS|CREATE_NO_WINDOW`, or thread
injection, got the exe quarantined by Defender.

### 8. Per-thread CPU

The runtime names its threads with `SetThreadDescription` ("N64 Thread n",
"Gfx Thread", "RT64 Workload", "RT64 Present", ...). Per-thread CPU time can be
read by opening each of the process's threads with access `0x0800`
(`THREAD_QUERY_LIMITED_INFORMATION`) and calling `GetThreadDescription` and
`GetThreadTimes`.

## Regenerating the recompiled code

`make regenerate` (in WSL) runs the pipeline in the one order that works:

| step | what it produces |
|---|---|
| `make resplit` | `asm/`, `assets/*.bin`, `ogrebattle64.ld`, `config/symbols/undefined_*_auto.txt` (splat) |
| `make` | `build/ogrebattle64.elf` |
| `make bank-recomp` | `Bank*Funcs/`, `app/src/bank_funcs.inc` (the 34 streamed-bank units) |
| `make recomp` | `RecompiledFuncs/` (N64Recomp plus the cross-bank dispatch) |
| `make rsp-recomp` | `RspFuncs/` (the njpeg and audio microcode) |

`bank-recomp` must run before `recomp`. The cross-bank dispatch
(`tools/cross_bank.py dispatch --only …`) rewrites the call sites that N64Recomp
binds to the wrong bank, and it sees only the bank records `bank_funcs.inc`
lists. Run the other way round, the build succeeds and runs wrong-bank code
(sessions 41/45/55); `make recomp` refuses to run in that state.

- `make resplit` leaves `asm/1CE040.s` and `asm/40E80.s` modified with identical
  content: a re-split writes the raw splat form, and
  `tools/fix_cross_overlay_labels.sh` re-applies the label patch on every build.
  Do not commit or revert those two files unless the label patch changes.
- Trees regenerated at different recompiler versions can differ in the
  `recomp_trace_return(rdram, N)` depth constants of 26 files under
  `Bank{B..L}Funcs/`. They feed the shadow call-chain diagnostic only.

## Third-party patches

`tools/N64ModernRuntime` and `tools/RT64` are checked out at pinned upstream
commits and carry the project's changes as uncommitted work. The tracked copy is
the patch files in `patches/` plus `tools/rt64-plume-sdl.patch`:

| build | source of the changes |
|---|---|
| a developer's machine | the dirty trees |
| a GitHub-hosted release runner (`vars.OGRE_DATA_REPO` set) | the patch files, applied to pristine checkouts of the same commits |

A patch that no longer matches its tree ships old code on the hosted path while
the build stays green (v0.5.0 and v0.5.1; session 116).
`python tools/patchcheck.py` (or `make patch-check`) exits 1 on any difference:
for every file a patch touches, `git diff HEAD -- <file>` must equal the patch's
block, and a second pass fails when the tree changes a file no patch carries.
`make patch-fix` rewrites stale patches; review `git diff patches/` before
committing. `tools/release-build.sh` runs the check before packaging.
`tools/rt64-plume-sdl.patch` has no post-image in the tree and is not checked.

Keep `.gitattributes`' `*.patch eol=lf` and `--ignore-whitespace` when editing a
patch or the workflow's `apply_patch` helper; without them the Windows job fails
at `rt64_tmem_hasher.h:203` and `rt64_native_target.cpp:369`.

## Build details

- **clang-cl under Ninja.** `cl.exe` rejects the GCC/Clang flag spellings the
  app, the runtime and RT64 use (`cl : command line error D8021: invalid numeric
  argument '/Wno-unused-variable'`). `app/CMakeLists.txt` adds `-Xclang
  -fexceptions -fcxx-exceptions` for clang-cl and keys its flag groups on
  `CMAKE_CXX_COMPILER_ID`, because CMake also reports `MSVC` for clang-cl.
- **Static SDL2 and CRT.** `DIST_STATIC_SDL` defaults to `1`, so `make app`
  builds `build-dist/` against SDL2 2.32.10 fetched into `tools/SDL2-static/`.
  On MSVC the library is `SDL2-static.lib` with its package config in
  `<prefix>/cmake`; `make` passes `-DOGRE_STATIC_SDL2=ON`. Every target and the
  SDL2 build use `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`. `--config Release`
  keeps a multi-config generator off Debug. `DIST_STATIC_SDL=0` builds
  `build-app/` against an installed SDL2, which the setup above does not
  provide.
- **Entry point.** The app defines `SDL_MAIN_HANDLED` and calls
  `SDL_SetMainReady()`. The exe is a GUI-subsystem image (`WIN32_EXECUTABLE`);
  `app/src/main.cpp` supplies the `WinMain` forwarder.
- **Windows branches.** `create_window` returns RT64's `{ HWND, thread_id }`.
  `readlink` and `__attribute__((weak))` have Windows branches.
- **DXC.** RT64 statically imports `dxcompiler.dll`/`dxil.dll`; its CMake copies
  them into `build-dist/RT64/`.
- **Frame pointers.** The app, the recompiled code and `librecomp`/`ultramodern`
  build with `-fno-omit-frame-pointer` (RT64 does not), so a debugger stopped in
  the runtime unwinds across a bridge to the guest caller.
- **Null renderer.** `cmake -S app -B build-null -DOGRE_USE_RT64=OFF` builds
  `null_renderer.cpp` with no GPU path. It has not been set up on this machine.
- **"the recompiled game code is missing"**: the files of "Windows setup" step 3
  are absent. `tools/release-build.sh` prints the same list.
- **Stale CMake cache.** After touching `tools/RT64/CMakeLists.txt` or the
  toolchain, delete `build-dist/` and rebuild.

## Running

```sh
OGRE_NO_AUDIO=1 "$APP" "$ROM"
```

With no ROM to boot the app opens its start screen: the
`OGRE BATTLE 64: RECOMP` title over a tabbed panel (MAIN, MODS, CONTROLS,
SETTINGS, DEBUG). `SPACE` or a click on the ROM row opens a file picker, and a
ROM can be dropped on the window. A ROM in the app's folder or `<app>/roms/` is
found automatically. The ROM is validated by XXH3 hash and a copy is stored in
the config directory, so later launches boot straight into the game, except that
the start screen still appears first when a mod is installed. `OGRE_LAUNCHER=1`
forces the start screen; `OGRE_ROM=<path>` names a ROM without an argument.

### Console output and logs

A double-click opens no console window. `main.cpp` attaches to the parent's
console with `AttachConsole` when one exists, so a terminal run prints its log,
and a stream the caller redirected to a file or a pipe keeps that target. A
launch with no standard handles leaves `stdout`/`stderr` bound to no descriptor
(`_fileno` is -2); `main.cpp` reopens them on `NUL` as fds 1 and 2, which lets
the crash log capture them. `OGRE_CONSOLE=1` calls `AllocConsole()` and points
the standard streams at a new console.

`OGRE_RSP_LOG=1` (the `[rsp]` task lines, read by `tools/runlog.py`) and
`OGRE_DL_TRACE=1` (the per-display-list lines, read by `tools/dlgaps.py`,
`tools/proflog.py --dl`, `tools/ogre_frame.py --log` and `runlog.py`'s cadence
line) are off by default. A stock run prints neither.

### Config directory

The config directory is the executable's own directory (`SDL_GetBasePath`):
`saves/<game id>.bin`, `mods/`, `mods.json`, `mod_config/`, `controls.cfg`,
`settings.cfg` and `error.log` sit beside the exe. `OGRE_PREF_DIR=<dir>`
overrides it. A read-only install location falls back to
`%APPDATA%\ogrebattle64\`. The boot log prints the resolved path.

### Paths under `/tmp`

Several built-in defaults are POSIX paths: the console file
`/tmp/ogre-console.txt`, the bare `dump`/`snap`/`save` outputs
(`/tmp/ogre-rdram-NNNN.bin`, `/tmp/ogre-snap-NNNN`,
`/tmp/ogre-checkpoint-NNNN.ckpt`), `snap`'s frames `/tmp/ogre-shot.<n>.ppm`, and
the `OGRE_RAM_KEY` output when neither `OGRE_RAM_KEY_OUT` nor `OGRE_PREF_DIR` is
set. The Windows CRT opens `/tmp/...` as `\tmp\...` on the current drive
(`C:\tmp\`), which must exist. Git Bash's `/tmp` is `%TEMP%`, a different
directory. Use explicit `C:/tmp/...` paths in `OGRE_*` values and console
commands; the examples below do.

### Closing the window

The close button makes SDL post `SDL_QUIT`; `update_gfx` writes any requested
dumps and calls `ultramodern::quit()`. Nothing joins the game's host threads, so
two changes keep the close from hanging (session 94): `librecomp/src/recomp.cpp`
does not free RDRAM when `exited` is set (`patches/n64modernruntime-ob64.patch`),
and `main.cpp` runs the SDL teardown and then `_Exit(EXIT_SUCCESS)`, after
`crash_log::flush()`, so no static destructor runs under a live game thread.
Teardown takes about half a second, exits 0 and writes no `error.log`. The
saving thread is joined inside `recomp::start` first, so the battery save is
safe.

## Distribution

### `make dist`

```sh
make dist DIST_OS=windows       # -> dist/ogre-battle-64-recomp/
make dist-zip DIST_OS=windows   # -> dist/ogre-battle-64-recomp-windows.zip
```

The package holds `ogrebattle64.exe`, `dxcompiler.dll`, `dxil.dll`,
`msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll` (and
`msvcp140_atomic_wait.dll` when present), `README.txt`, `mods/*.nrm` and an
empty `mod_config/`. SDL2 and the CRT are static.

The MSVC runtime DLLs are required: `dxcompiler.dll`/`dxil.dll` import
`MSVCP140.dll`/`VCRUNTIME140.dll`, so a machine without the Visual C++
redistributable cannot start the process. `app/CMakeLists.txt` copies the
app-local runtime from `VCToolsRedistDir`, and `make dist` fails when a DLL is
missing. It also needs the generated code and at least one `build/mods/*.nrm`.
`dist-zip` uses `zip` when present and otherwise CMake's zip writer.

Acceptance test: launch the package in an empty folder; the start screen
appears, a chosen or dropped ROM boots the game, and
`saves/ogrebattle64-us-rev1.bin` is written beside the exe.

### Smoke test

```sh
DIST_OS=windows /usr/bin/bash tools/smoke-dist.sh [dist-dir]
```

`make smoke` runs the same script through its `#!/usr/bin/env bash` line, which
finds WSL's `bash` when the VS `PATH` is first; call `/usr/bin/bash` directly.
`tools/smoke-dist.sh` runs on the machine that built the package, and the
release workflow runs it before collecting the archive. It checks:

1. the exe, `README.txt`, the companion DLLs and at least one `mods/*.nrm` exist;
2. `tools/pe_imports.py` finds every DLL the shipped `.exe`/`.dll` import either
   shipped or a Windows system DLL (a launch on a runner cannot catch this,
   because the runner has the redistributable);
3. the exe reaches `main` with `OGRE_SMOKE=1` within `SMOKE_TIMEOUT` seconds
   (default 90);
4. a run with `OGRE_CRASH_TEST=segv` writes an `error.log` that contains the
   `[boot]` line printed just before the raise;
5. on a host with an Explorer shell, a launch with no standard handles writes
   the same report: the script writes a `.lnk` that passes `--crash-test=segv`,
   opens it with `explorer.exe`, and waits for the process to exit. A host with
   no Explorer (a hosted runner, a service) prints a skip line.

`SMOKE_TIMEOUT` bounds checks 3 and 5. The script resolves `python3` for
`tools/pe_imports.py`; the Microsoft Store stub fails check 2 (see "Traps on this
machine").

### Crash reports — `error.log`

`crash_log::install()` (`app/src/crash_log.cpp`) runs at the top of `main()`:

- It replaces fd 1 and fd 2 with pipes, each drained by a thread into a 128 KiB
  ring and forwarded to the real descriptor. `stdout` gets line buffering with a
  static buffer and a non-zero size: the MSVC CRT treats
  `setvbuf(stream, nullptr, _IOLBF, 0)` as an invalid parameter and fast-fails
  (`0xC0000409`). The handlers are installed before the capture.
- It installs `SIGSEGV`/`SIGABRT`/`SIGFPE`/`SIGILL` handlers through `signal`,
  `SetUnhandledExceptionFilter`, and `std::set_terminate`.

A fatal event writes `<config dir>/error.log`: the reason, the faulting
instruction and the address it touched (with host module and offset, and read,
write or execute), the RDRAM base, the N64 thread, the last recompiled function
per thread, the last 200 stderr and 100 stdout lines. The handler uses raw
writes and never `fflush`, because the faulting thread can hold a stdio lock.
Windows has no `ftrylockfile`, so the handler uses no stdio at all there and
skips the runtime's printf-based call-chain dump. Only the first report is
written. `OGRE_DUMP_RDRAM=<path>` adds the 8 MiB image. `crash_log::flush()`
restores the descriptors; `atexit` and the `_Exit` paths call it.

A hang writes `error.log` too. `app/src/hang_watch.cpp` runs from `update_gfx`
on the main thread and reads the game's frame counter `D_800AEFA4` (the VI count
copied at the end of each game frame, `0x800726C4`). When the counter stays
still for `OGRE_HANG_MS` (default 5000; `0` turns the watch off), it prints a
`[hang]` line with the frame counter, the VI count `D_800C4BCC` and the scene
`D_800E810E`, runs the `[snap]` queue snapshot once (the main thread stalls about
1.6 s) and writes `error.log` with the reason `hang: …`. The stdout section then
holds the `[hang]` line and the snapshot. It reports once per stall and prints
`[hang] frames resumed …` if frames come back. The boot (until the counter has
changed twice), time with the game window minimized or hidden, and any gap over
1 s between two main-thread calls do not count. With `OGRE_HANG_MS=150` the
scene loads at `0x09`, `0x0A` and `0x04` trigger it, which is a quick way to see
a report.

The runtime's `message_box` errors also write `error.log`.
`report_boot_failure()` (`app/src/sdl_platform.cpp`) writes `error.log` and
shows a message box when SDL init, the game window or the start screen's window
or renderer fails. `init_sdl` requires only video and events; audio and
controllers may be absent, and the game then runs silently.

## Releases (GitHub Actions)

`.github/workflows/release.yml` builds packages, creates a GitHub release and
attaches one archive per platform (`ogre-battle-64-recomp-windows-x86_64.zip`
for Windows). Its matrix still lists macOS and Linux.

### `make release`

```sh
make release VERSION=v0.6.0
gh release edit v0.6.0 --draft=false     # publish after reviewing the assets
```

`tools/release.sh` regenerates the game code (`make bank-recomp`,
`make recomp`, `make rsp-recomp`) and the example mods, packs `files.tar.gz`
with `tools/data-bundle.sh` and compares it with the tree, commits and pushes it
to the private data repository, and dispatches the Release workflow as a draft.
It regenerates, so it runs where the ROM and the MIPS toolchain are (WSL). It
refuses a version that already has a tag or release, a tracked tree with
uncommitted changes, and an unpushed branch tip, because the bundle records
`git rev-parse HEAD`. `make release-check VERSION=…` is the same with
`NO_PUSH=1` and changes nothing outside the tree.

```sh
VERSION=v0.6.0          # default: newest tag + 1
DATA_REPO=OWNER/REPO    # default: the OGRE_DATA_REPO Actions variable
REGEN=0                 # use the generated code already in the tree
SKIP_BUNDLE=1           # reuse dist/ogre-data/files.tar.gz (a retry)
DRAFT=false             # publish instead of drafting
NO_WAIT=1               # do not wait for the run
make release
```

A commit that changes only tooling or docs leaves the generated code identical;
the script then repacks the archive with the new commit, because the workflow
refuses a bundle whose recorded commit differs.

**Windows only.** The `platforms` input selects the matrix of a dispatched run
(`all`, `windows`, `linux`, `macos`). A Windows run takes about 8 minutes, runs
the smoke test and creates a release with only the Windows asset. `make release`
always asks for `all`, and a tag push builds all and publishes.

```sh
gh workflow run release.yml -f tag=v0.2.1-rc3 -f platforms=windows
```

`tools/release-build.sh` is the single build entry point; the workflow exports
`DIST_OS` and `CMAKE_ARGS` and runs it, so a release artifact is what `make dist`
produces locally.

### The data bundle

A hosted runner has no ROM. With the Actions variable `OGRE_DATA_REPO` (the
private repository slug) and the secret `OGRE_DATA_TOKEN` (fine-grained,
`Contents: Read`) set, every job runs on its hosted runner and unpacks
`files.tar.gz` from that repository. With the variable unset the jobs use
self-hosted runners (`["self-hosted","Windows","X64"]`).

- The archive holds `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/`,
  `app/src/bank_funcs.inc`, `build/mods/*.nrm`, and `ogre-data.txt` with the
  commit it was generated from.
- The mods are in it because a runner cannot build them. `make dist`,
  `tools/release-build.sh`, the unpack step and `tools/smoke-dist.sh` each fail
  when they are absent (v0.4.0 shipped an empty `mods/`; session 108).
- The unpack step fails when the recorded commit differs from the commit being
  built. Regenerate the bundle for every release.
- `tools/data-bundle.sh` excludes AppleDouble `._*` members and the unpack step
  deletes any: `app/CMakeLists.txt` globs `*.c`.

On the hosted Windows job the workflow clones N64ModernRuntime at
`589bbf018a3e6d3646ddf7de1e7919f1b7e99bb1`, applies the runtime and RT64/plume
patches with `--ignore-whitespace` (`apply_patch` skips one already applied),
runs `choco install make ninja`, sets up MSVC with `ilammy/msvc-dev-cmd@v1`
(`arch: x64`), and builds with the clang-cl `CMAKE_ARGS`. A self-hosted Windows
runner needs Visual Studio 2022 with the C++ Clang tools, `ninja` and `make` in
a developer environment.

## Controls and settings

### Controller bindings

Bindings are data (`app/src/input_map.cpp`), edited from the CONTROLS tab of the
start screen and the `ESC` overlay, and saved as text at `<config>/controls.cfg`
(`<n64 tag>.<key|pad> = <value>`; `-` and `none` are unbound; key names are
`SDL_GetScancodeName`'s). The input callbacks read the map on every poll.

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
| C-LEFT / C-RIGHT | `J` / `L` | (none; the right stick) |

The left stick is always the N64 analog stick, and the right stick presses the C
buttons on top of the map. `ESC` is reserved for the overlay. `=`/`-` and the
pad's R3 change the game speed and are outside the map.

The game reads N64 controller 1 only. The first connected pad fills
`Platform::controllers[0]` and the keyboard is OR'd into it; a second pad fills
slot 1, which the game never polls. The app enumerates connected pads once,
skips SDL's startup `SDL_CONTROLLERDEVICEADDED` for a pad it holds, and prints
one `[input] controller <n>: <name>` line per slot (issue #13).

### The CONTROLS tab

```
A          KEY X            PAD A              - Selects the highlighted object.
…
Reset bindings to defaults   Restore the default keyboard and gamepad map.
MENU PAD NAVIGATION: D-PAD MOVE, A SELECT, SELECT MENU, LB/RB SWITCH TAB
```

`SPACE` arms a capture; the next keydown fills the key slot and a pad button the
pad slot. `BACKSPACE` clears the key, `DELETE` the pad, `ESC` cancels.
`TAB`/`SHIFT+TAB` or a click switch tabs. In the panel, `D-PAD` moves and steps,
`A`/`START` activate, `LB`/`RB` switch tab (`Panel::pad_command_for_button` in
`app/src/ui.cpp`, shared by launcher and overlay). Pad events are
`SDL_CONTROLLERBUTTONDOWN` and do not repeat. The pad's SELECT
(`SDL_CONTROLLER_BUTTON_BACK`) toggles the overlay while the map binds nothing to
it (`pad_button_is_bound`).

### GAME SPEED

GAME SPEED is the emulated-clock multiplier `OGRE_SPEED` also sets: the CPU
counter and the VI retrace schedule run `n` times faster. The row offers 1, 2, 4
and 8, applies immediately (also from the overlay), and is saved as
`game_speed = <n>` (`app/src/settings.cpp`). A value the row does not offer loads
as the closest offered one. `ultramodern::set_speed_multiplier()` (`timer.cpp`)
keeps the clock piecewise linear: `speed_start_us`/`speed_start_ticks` anchor
the current multiplier, published under a seqlock.

- `=`/`-` (keypad `+`/`-` too) step 1 → 2 → 4 → 8 and stop at the ends; a
  starting value outside the table steps from that value. Read in
  `pump_sdl_events`, ignored while the `ESC` panel is up.
- R3 cycles the table and wraps 8 → 1. It observes the event, so an R3 bound to
  an N64 button still presses it. Ignored while the panel is open.
- `app/src/speed_badge.cpp` shows `1x`..`8x` in the game window's lower-right
  corner for two seconds after a change, as a borderless always-on-top SDL window
  (0.85 opacity). It is not drawn in fullscreen and hides under the pointer,
  because SDL2's hit test has no click-through.

### WIDESCREEN

```
WIDESCREEN   [ ] OFF [x] ON
```

WIDESCREEN puts RT64 into its Expand aspect ratio, which widens the field of
view: `rt64_workload_queue.cpp` derives `aspectRatioScale` from the window and
`ProjectionProcessor::processScene` counter-scales screen-covering projections.
`ON` turns Expand on only while the dispatcher's scene (`D_800E810E`) is in the
widescreen set: `0x03` (mission) and `0x0D` (dialogue/cutscene) by default
(`OGRE_WS_SCENES`), plus every scene the HD-background pack covers. The 2D
screens are authored for 4:3 and are pillarboxed. `app/src/widescreen.cpp` is
the only writer of `ar_option`, once per frame from `update_gfx`, and writes only
on a difference. Saved as `widescreen = off|on`; the retired `missions`,
`always`, `1` and `2` read as `on`.

The window's shape follows the toggle and never changes with the scene.
`create_window` asks `widescreen_initial_window_size`, and
`widescreen_fit_window` re-derives the width from the height the window got.
`OFF` opens 4:3; `ON` opens 16:9 and pillarboxes 4:3 scenes. Only a toggle change
re-fits the window (`widescreen_update` reports it to `main.cpp`). The `MISSION`
banner clipping at both edges is the game's own marquee and happens in 4:3 too.

### Display: WINDOW, RESOLUTION, MSAA

```
DISPLAY
WIDESCREEN   [x] OFF [ ] ON
RESOLUTION   [ ] NATIVE [ ] 2X [x] AUTO
MSAA         [x] OFF [ ] 2X [ ] 4X [ ] 8X      (only with OGRE_DISPLAY_EXPERIMENTS=1)
WINDOW       [x] WINDOWED [ ] FULLSCREEN
```

The game window opens 1080 points tall (1440x1080, or 1920x1080 with WIDESCREEN
on), clamped to the primary display's usable area. The process is per-monitor
DPI aware (`SDL_HINT_WINDOWS_DPI_SCALING` in `init_sdl`), so window sizes are
points and the swap chain is real pixels. The boot logs
`[SDL] game window <points> points, <pixels> pixels`.

| row | options | what it changes |
|---|---|---|
| `RESOLUTION` | `NATIVE` / `2X` / `AUTO` | `res_option`. `NATIVE` is RT64's `Original` (320x240); `2X` is `Original2x`, which `set_application_user_config` (`app/src/renderer.cpp`) turns into `Manual` at `resolutionMultiplier 2.0`; `AUTO` (default) is `WindowIntegerScale`, the largest integer multiple of 320x240 that fits the window's pixels (4x at 100% scaling on 1080p, 9x at 200%) |
| `MSAA` | `OFF` / `2X` / `4X` / `8X` | `msaa_option`; a change rebuilds the render targets (`updateMultisampling`). Experimental |
| `WINDOW` | `WINDOWED` / `FULLSCREEN` | `wm_option`; the backend's `setFullScreen`, without recreating the window |

1:1 2D screens (publishers, title, text) look the same at every resolution
because RT64's `upscale2D = ScaledOnly` keeps unscaled rectangles native; scaled
sprites, njpeg backdrops and 3D render higher. Saved keys:
`render_resolution = native|2x|auto`, `msaa = off|2x|4x|8x`,
`window = windowed|fullscreen`. The older `resolution` key is not read, so an
existing `settings.cfg` gets `AUTO`.

MSAA is gated by `OGRE_DISPLAY_EXPERIMENTS` (`1`/`on`/`true`/`yes`) or a set
`OGRE_MSAA`. With the gate off the row is absent (`Panel::build_rows`), the value
is not applied (`applied_antialias`; the boot prints `msaa 4x is experimental and
not applied; set OGRE_DISPLAY_EXPERIMENTS=1 to enable it`), and a stored value
is kept.

RT64's fullscreen is a raw `SetWindowPos` to the monitor rect, outside SDL, so
`widescreen_fit_window` returns early while the config asks for fullscreen
(`[widescreen] fullscreen: the renderer owns the window's shape`). On leaving
fullscreen `RT64Renderer::update_config` calls `widescreen_request_refit()`, so
a WIDESCREEN change made in fullscreen applies on the way out.

All rows apply through `RT64Renderer::update_config` on the next frame.
`settings.cpp` and `widescreen.cpp` each start from the live `GraphicsConfig` and
change only their own fields. Each change prints
`[renderer] config: resolution 2x (mult 2.00), msaa 4x, window fullscreen`.
The graphics API is fixed at renderer creation and stays an environment
variable (`OGRE_GRAPHICS_API`).

### SOUNDS and VOLUME

```
AUDIO
SOUNDS   [x] ON [ ] OFF
VOLUME   ----------| 100%
```

VOLUME moves 5% per press and clamps; a click sets a slider cell (11 cells, 10%
each). The gain is applied in `queue_audio_samples` (`sdl_platform.cpp`): 100
queues the buffer unchanged, a lower gain scales a copy, 0 queues silence. The
frame count is the same at every gain, because `ultramodern::audio_dma_busy()`
paces the game's audio from the queued depth. Saved as `sounds = off|on` and
`volume = <0..100>`; a file without the keys loads `ON` / `100`.

### MOUSE ENABLED

```
INPUT
MOUSE ENABLED   [x] OFF [ ] ON
```

`ON` lets the pointer drive the world map's and the mission field's cursors (see
"Mouse cursors"). `OFF` (default) leaves the OS pointer visible. The row is
published to `ogre_map_cursor_set_enabled` and `ogre_mission_cursor_set_enabled`
on every change. Saved as `mouse = off|on`.

### The in-game overlay

`ESC` during play opens a borderless, always-on-top SDL window over the game
window (`app/src/overlay.cpp`) drawing the same `ui::Panel`. MODS is greyed
(mods load before boot). MAIN carries EXIT GAME. While open, `get_input` reports
an idle controller and the game keeps running. The window is created on the
first `ESC`, synced to the game window each frame, with
`SDL_SetWindowOpacity` (default 0.90).

EXIT GAME sets a request that `update_gfx` polls beside the window-close flag,
so both exits share one teardown. Both check the game's frame counter
(`D_800AEFA4`): before the first frame they leave with `_Exit(0)`, because
`ultramodern::quit` during `ultramodern::preinit` or RT64's construction blocks
in `game_thread.join` or throws.

The hidden overlay stays in SDL's window list, and SDL synthesises `SDL_QUIT`
only for the last window's close. `pump_sdl_events` therefore quits on a close
event whose `windowID` is the game window's, and ignores one for the overlay.

### Verifying the UI without a human

```sh
# every launcher tab, as a PPM
for tab in main mods controls settings debug; do
  OGRE_NO_AUDIO=1 OGRE_PREF_DIR=C:/tmp/ui OGRE_LAUNCHER=1 OGRE_LAUNCHER_TAB=$tab \
    OGRE_LAUNCHER_SHOT=C:/tmp/ui-$tab.ppm "$APP"
done

# rebind A to P through the launcher's key handler
OGRE_NO_AUDIO=1 OGRE_PREF_DIR=C:/tmp/ui OGRE_LAUNCHER=1 OGRE_LAUNCHER_KEYS="Tab,Tab,Tab,Space,p" \
  OGRE_LAUNCHER_SHOT=C:/tmp/ui-capture.ppm "$APP"
grep '^a.key' /c/tmp/ui/controls.cfg            # a.key = P

# GAME SPEED 1 -> 4 in the overlay while the game runs
OGRE_NO_AUDIO=1 OGRE_PREF_DIR=C:/tmp/gs OGRE_ROM="$ROM" OGRE_OVERLAY_AT_MS=2500 \
  OGRE_OVERLAY_TAB=settings OGRE_OVERLAY_KEYS="Right,Right" \
  OGRE_CAPTURE_OVERLAY=C:/tmp/gs.ppm OGRE_OVERLAY_SHOT_MS=1200 OGRE_EXIT_AFTER_MS=7000 "$APP"
grep '^game_speed' /c/tmp/gs/settings.cfg       # game_speed = 4

# a pad opens the overlay and steps a SETTINGS row; the game sees an idle pad
OGRE_NO_AUDIO=1 OGRE_PREF_DIR=C:/tmp/pad OGRE_ROM="$ROM" OGRE_OVERLAY_TAB=settings \
  OGRE_OVERLAY_PAD="back,ddown,ddown,dright" OGRE_CAPTURE_OVERLAY=C:/tmp/pad.ppm \
  OGRE_OVERLAY_SHOT_MS=2500 OGRE_EXIT_AFTER_MS=14000 "$APP"
# "[overlay] shown", "[renderer] config: ...", "[input] overlay open: game input suppressed"

# EXIT GAME, and the close button after the overlay opened and closed
OGRE_NO_AUDIO=1 OGRE_PREF_DIR=C:/tmp/ex OGRE_ROM="$ROM" OGRE_OVERLAY=1 \
  OGRE_OVERLAY_TAB=main OGRE_OVERLAY_KEYS="Space" OGRE_EXIT_AFTER_MS=60000 "$APP"
OGRE_NO_AUDIO=1 OGRE_PREF_DIR=C:/tmp/cl OGRE_ROM="$ROM" OGRE_OVERLAY=1 \
  OGRE_OVERLAY_KEYS="Escape" OGRE_WINDOW_CLOSE_AT_MS=9000 OGRE_EXIT_AFTER_MS=60000 "$APP"
# "[overlay] EXIT GAME" / "[SDL] game window close requested", exit 0, no error.log
```

The SETTINGS rows are GAME SPEED, WIDESCREEN, RESOLUTION, (MSAA), WINDOW,
SOUNDS, VOLUME, MOUSE ENABLED, with headings skipped, so a script's `Down` count
depends on the MSAA gate.

## Mouse cursors

A click ORs the N64 `A` bit into controller 0 for a few pad polls in
`get_input`, the path the keyboard and the console's `press` use. One click arms
both screens' confirm, and each count is consumed on every poll, so an `A` armed
on one screen cannot fire on the other. `pump_sdl_events` hides the OS pointer
over the game window and shows it over the overlay or outside, from
`SDL_GetMouseFocus()` and `overlay_visible()`; the close path restores it.

**World map (scene `0x05`).** The cursor is a sprite at screen pixels in the map
state object (`*(u32*)0x80197B18`): `func_ovlM_8019AF0C` keeps
`state+0x52`/`+0x54` clamped to x `0x28..0x110`, y `0x28..0xC0`, and the cursor
draw `func_ovlM_801A103C` reads the pair at `0x801A1C48`/`0x801A1C4C`.
`app/src/map_cursor.cpp` aspect-fits the 320x240 framebuffer in the drawable and
writes the pointer's pixel there; `tools/map_cursor.py` inserts the call into
the bank unit and `make bank-recomp` re-applies it. A pointer within 24 px of a
bound holds the matching D-pad bit, which scrolls the view, also from the
pillarbox or a fullscreen border. The map also reads the analog stick at
`*(0x800C47F0)+2`/`+3`.

**Mission field (scene `0x03`).** The cursor is two f32 framebuffer pixels at
`state+0x08`/`+0x0C`, `state = *(u32*)0x801AB870 = 0x800E91D0` (a fixed main-ELF
BSS address the mission modules hold as a constant). The mover
`func_ovlN_801BE4E0` (unit N, record 7) reads the held buttons `*(0x800E8108)`
and the D-pad masks of the profile at `*(0x801F0CC0)`, integrates a ramped delta,
stores the pair at `0x801BE974`/`0x801BE988`, and clamps it to x 40..280,
y 30..210. It is D-pad driven; no mission bank reads the stick. The game moves
this cursor every frame (its own integration, a pull to centre, snap-to-unit,
command recentring), so the pointer adopts the game's position in these cases
and places the cursor otherwise:

| the game moves the cursor when | log |
|---|---|
| a D-pad direction is held | `pad=1` |
| C-LEFT is held in a centring mode (profile `+0x1C`, field mode `*(0x801F0E00)` < 2 or == 42) | `centre=1` |
| the mover integrated a delta this call (`state+0x06`/`+0x07`) | `nudge=1` |
| the window after a click is running | `yield=1` |

`tools/mission_cursor.py` inserts `ogre_mission_cursor_note_move` at
`L_801BEC04` (the last point where the delta bytes hold this call's values) and
`ogre_mission_cursor` at the epilogue `L_801BECC8`. The camera pans with the
cursor: `func_ovlN_801BECF0` adds `delta/128 * 8.5 * table[e]` to the eye and
look-at of the camera at `*(u32*)0x8018F5AC` while `*(0x801F3630)` is set. A
pointer near a play-area bound holds the D-pad bit (right `0x100`, left `0x200`,
up `0x800`, down `0x400`) to push the view.

| Variable | Effect |
|---|---|
| `OGRE_MOUSE=off\|on` | MOUSE ENABLED for one run |
| `OGRE_MAP_CURSOR=1\|0`, `OGRE_MISSION_CURSOR=1\|0` | force one screen's cursor on / off (`off`, `false`, `no` mean 0) |
| `OGRE_MAP_CURSOR_LOG=1` | `[mapcursor]` per change of cursor, pointer, draw flag or edge push; `[cursor] hidden/shown` per OS pointer change |
| `OGRE_MISSION_CURSOR_LOG=1` | `[missioncursor]` per change of cursor, pointer, D-pad or camera (`cam=<eye x>,<eye z>/<look x>,<look z>`, `edge=` mask), per edge push, confirm poll and click |
| `OGRE_MAP_CURSOR_CONFIRM_POLLS`, `OGRE_MISSION_CURSOR_CONFIRM_POLLS=<n>` | pad polls a click holds `A` (default 8) |
| `OGRE_MAP_CURSOR_EDGE`, `OGRE_MISSION_CURSOR_EDGE=<n>` | edge distance that pushes (default 24 px) |
| `OGRE_MAP_CURSOR_EDGE_FORCE`, `OGRE_MISSION_CURSOR_EDGE_FORCE=<mask>` | hold that D-pad mask with no mouse event |
| `OGRE_MISSION_CURSOR_YIELD=<n>` | frames after a click during which the pointer adopts the game's cursor (default 15), covering the camera settling |
| `OGRE_MISSION_CURSOR_AT=<x>,<y>` | park the pointer at that framebuffer pixel once; not with a real mouse |
| `OGRE_CURSOR=0` | keep the OS pointer visible over the game window |

## Mods

The runtime (`librecomp/src/mods.cpp`) opens every mod, recompiles a code mod's
MIPS at load time, applies its hooks and replacements, and persists toggles and
options. `app/src/main.cpp` sets `GameEntry::mod_game_id` to `MOD_GAME_ID`
(`app/src/game.hpp`) and calls `initialize_mods()` + `scan_mods()` before the
start screen; the scan inside `recomp::start` re-reads `mods.json`.

| path | what |
|---|---|
| `<config>/mods/*.nrm` | one file per mod |
| `<config>/mod_config/<mod id>.json` | that mod's option values |
| `<config>/mods.json` | enabled mods and their order |

### The start screen

`app/src/launcher.cpp` draws the shared `ui::Panel` (`app/src/ui.cpp`):

```
MAIN         Start Game (selectable once a ROM is loaded), the ROM row (opens
             the picker), Exit Game (last, so a stray SPACE or ENTER misses it)
MODS         one row per mod, then one per visible option
CONTROLS     one row per N64 button: key, pad source, field-map action
SETTINGS     GAME SPEED; DISPLAY; AUDIO; INPUT
DEBUG        the live Chaos Frame readout

TAB / SHIFT+TAB  switch tab          UP / DOWN     select a row
SPACE            activate the row    LEFT / RIGHT  step a value
ENTER            play, or open the picker with no ROM or on the ROM row
mouse            click a tab, a row or a marker; elsewhere plays
```

Choosing a ROM moves the selection to Start Game without starting. A toggle
calls `recomp::mods::enable_mod` (writes `mods.json`); an option calls
`recomp::mods::set_mod_config_value` (writes `mod_config/<mod id>.json`).
Deleting a `.nrm` removes a mod.

### Building an example mod

`mods/skip-boot-logos/` is the reference: one code mod with one entry hook on a
scene update. It writes the pending-scene word from scene `0x09`'s update, so
the boot skips the intro and the publisher stills (~26 s); its manifest sets
`enabled_by_default = false`, which `RecompModTool` emits
(`patches/n64recomp-ob64.patch`). Skip from `0x09`: writing the word from
`0x0A`'s update leaves overlay-C globals that only `0x0A`'s completed state
machine makes consistent, and the title then produces no display lists.

```sh
# WSL
make mod-syms        # -> mods/reference/{dump,data_dump}.toml
make example-mods    # -> build/mods/*.nrm
```

`make mod-syms` runs N64Recomp with `--dump-context` on
`build/ogrebattle64.elf`; the mod tool resolves hooked names against it.
`tools/build-example-mods.sh` compiles each mod's `src/*.c` with
`mips-linux-gnu-gcc` (or a `debian:bookworm-slim` container when it is absent)
and runs `RecompModTool`. Copy `build/mods/*.nrm` to the Windows checkout.

### What a hook can reach

A hook is a section ROM address plus function vram, resolved through
`get_vrom_to_section_map()`: the ELF's own sections registered by
`register_base_overlays()` (`.entry`, `.main`, `.streamedA/B/C`). A hook in a
streamed bank unit (`app/src/bank_overlays.cpp`) is outside that map and
unproven. Bank-code insertions (the cursors, the HD backgrounds) go in through a
generated-unit patch script instead.

`patches/n64modernruntime-ob64.patch` carries the mod-loader fixes this needs:
`context_from_regenerated_list` translates the recompiler's ELF section index
(`get_code_section_index_from_section_index`; without it a mod fails with
`Failed to load mod code (Code mod loading internal error)`), and a HI16/LO16
relocation against a non-code section stays the absolute immediate it already
is. The same patch makes `debug_dump_queue_snapshot` sign-extend 32-bit guest
addresses, so a `[snap]` line with an impossible count is a report about the
game.

## Saves

### The cartridge battery (SRAM)

OB64 saves to 32 KiB of battery SRAM (`entry.save_type =
recomp::SaveType::Sram`); the Controller Pak strings belong to the game's
copy/backup feature. The image is `<config>/saves/ogrebattle64-us-rev1.bin`,
32768 bytes. `func_8008BC40` queues an `OSIoMesg` to `D_800AA408`, and
`pi_perform_dma` (`librecomp/src/pi.cpp`) completes it inline, choosing the
device from the message's `OSPiHandle` (save handle `baseAddress 0xA8000000`,
cart `0xA0000000`). The game reads the image at boot in 256-byte DMAs
(`func_80074CF0`) and writes it back when dirty (`func_80074BF0` →
`func_80074C58`). Slots are `0x1850` bytes at `0x10 + n*0x1850`; the signature
is ASCII `QuestOG3`.

A slot's first 4 bytes are two `u16` checksums seeded with the slot's device
offset; `func_8007541C` rejects a slot unless `slot+4` holds `QuestOG3` and

```
u16[+0x00] == (sum of the bytes of slot+0x0C .. slot+0x1850) + (0x10 + n*0x1850)   // func_80075A84
u16[+0x02] == (count of their set bits)                     + (0x10 + n*0x1850)   // func_80075B00
```

A slot is valid only at the index it was checksummed for. The copy-to-pak path
uses unseeded twins (`func_80075AC4`/`func_80075B60`), so a slot from a
Controller Pak must be reseeded; an unreseeded import makes the game reformat
the image. The runtime prints `[save]` lines on load or create, the first read,
the first write and `[save] wrote <path>`. A failed write raises a message box
from the saving thread and stalls the run; use a writable `OGRE_PREF_DIR`.

### Importing emulator saves

```sh
tools/sramsave.py check  save.srm                      # what is in the file; ok / MISMATCH per slot
tools/sramsave.py import save.srm <config>/saves/ogrebattle64-us-rev1.bin
tools/sramsave.py export <that .bin> out.sra           # 32-bit byteswapped
tools/sramsave.py pak --all assets/saves/prologue.n64  # list a pak's notes
```

`import` finds the SRAM by its magic at any offset and byte order and refuses a
file without it (parallel-n64: 296960 bytes, SRAM at `0x20800`, words reversed;
mupen64plus `.srm`: the bare 32 KiB). DexDrive `.N64` files (`assets/saves/*.n64`)
are a 0x1040-byte header plus a 32 KiB Controller Pak holding the game's backup
notes (`NOBE`/`EB`, `OgreBATTLE64 <n>`), each with a verbatim battery slot at
`note+0x20`. `import` follows the pak's FAT to the live notes and reseeds them
into battery slots 0 and 1. The battery has two save slots: slot 2's offset
`0x30B0` is the game's 19176-byte map record. `--all` also takes deleted notes.

### Running with a save — `OGRE_SAVE`

```sh
OGRE_NO_AUDIO=1 OGRE_SAVE=prologue OGRE_SCENE=title "$APP" "$ROM"
OGRE_NO_AUDIO=1 OGRE_SAVE=C:/saves/save.srm OGRE_SAVE_RESET=1 "$APP" "$ROM"
```

| Variable | Effect |
|---|---|
| `OGRE_SAVE=<name\|path>` | use that save as the battery. A bare name is looked up as a path, then `<rom dir>/saves/<name>[.n64\|.bin]`, then `assets/saves/<name>[.n64\|.bin]`. Accepts everything `sramsave.py import` does. The result goes to `<config>/saves/<game id>.bin`, never back into the source |
| `OGRE_SAVE_RESET=1` | re-import even when the config dir's battery is newer than the source; without it a run keeps its progress |
| `OGRE_SAVE_ALL=1` | also take deleted notes (still two slots) |

`tools/run-save.sh <name>` gives each save its own config dir
(`.ogre-prefs-save-<name>/`, gitignored) with `--reset`, `--list` and
`--import-only`. It runs `OGRE_APP` if set, else the first of
`build-dist/ogrebattle64.exe`, `build-app/ogrebattle64.exe`, `build-dist/ogrebattle64`
and `build-app/ogrebattle64` that exists, and copies `dxcompiler.dll`/`dxil.dll`
beside a `.exe` from its `RT64/` directory. Arguments after the save go to the
app, so pass the ROM there when `assets/ogre64.z64` is absent:
`/usr/bin/bash tools/run-save.sh <save> <rom>`.

The Load Game book (scene `0x12`) shows which battery the game read: with
`prologue.n64` imported, GAME DATA 1 reads `Magnus / Prologue / Alba / 0:20:42`.
A blank battery also reaches `0x12` and `0x05`, so the route alone proves
nothing.

## Environment knobs

All knobs default to off. Settings overrides apply for one run and never write
`settings.cfg`.

### Run control and scripted input

| Variable | Effect |
|---|---|
| `OGRE_NO_AUDIO=1` | open no audio output device (`[SDL] OGRE_NO_AUDIO: no output device opened`). Set on every test run. The audio microcode still runs |
| `OGRE_EXIT_AFTER_MS=<n>` | after `n` ms print the last recompiled function and live call chain of every game thread, then exit 0 through `_Exit` (a graceful teardown mid-call segfaults intermittently) |
| `OGRE_PREF_DIR=<dir>` | config dir |
| `OGRE_ROM=<path>` | boot this ROM without an argument |
| `OGRE_LAUNCHER=1` | force the start screen |
| `OGRE_TEST_DROP=<path>` | one synthetic ROM drop to the start screen |
| `OGRE_SCENE=<name\|hex>` | boot into a scene; see "Scene forcing" |
| `OGRE_SCENE_AFTER_MS=<n>` | delay before the first scene request (default 0) |
| `OGRE_FORCE_SCENE=<hex>` | the original hex-only spelling of `OGRE_SCENE`; `_AFTER_MS` likewise |
| `OGRE_STEP=<n>` | seed the New Game step word `D_8018F1C0` once when the target scene is active, released when the step moves or after 1500 ms. It changes the sequence's state: entering `0x0D` at step 2 without step 1 makes the exit re-enter at step 0, the movie-mode branch, and crash. Drive the title with taps for a real New Game |
| `OGRE_SMOKE=1` | print `[smoke] main reached`, try SDL once, exit 0 |
| `OGRE_CRASH_TEST=<segv\|bus\|abrt\|fpe\|ill>` | raise that signal right after the crash logger is armed. The argument `--crash-test=<name>` does the same for a launch that cannot carry the variable (a shortcut started by Explorer); an unknown name in the argument exits 1 |
| `OGRE_CONSOLE=1` | `AllocConsole()` for the standard streams |
| `OGRE_LIVE_CONSOLE=1` | the live console; see "The live console" |
| `OGRE_TAP_MS=<n>` | controller 0 presses Start (or `OGRE_TAP_BUTTON`) for 150 ms every `n` ms |
| `OGRE_TAP_BUTTON=<list>` | buttons the taps press, one per tap (`start`, `a`, `b`, `down`, …; `none` skips). On the name-entry form (`0x07`) a tap in a text field opens a tooltip that blocks the sequence; the working shape is a few `start,a` pairs then a long run of `a` (`"start,a,start,a,start,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a"`). An endless `start,a` stalls on `0x07` |
| `OGRE_TAP_SCENE=<list>` / `OGRE_TAP_NOT_SCENE=<list>` | tap only while / except while `D_800E810E` is in the list (`kScenes` names or hex). `OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game` presses through the title and stops at New Game |
| `OGRE_TAP_SCENE_BUTTON=<scene>:<button>[:<count>],…` | press that button every `OGRE_TAP_MS` while that scene runs, at most `<count>` times, in order per scene; replaces the slot schedule. `"title:start:5,0x12:a:3,0x05:right:3,0x05:a:1"` |
| `OGRE_LAUNCHER_TAB` / `_KEYS` / `_PAD` / `_SHOT` / `_SHOT_MS` | start-screen tab; synthetic keydowns (SDL scancode names) or pad presses (`controls.cfg` tags) every 150 ms through the real handlers; write a PPM and quit (after the scripts, or after `_SHOT_MS`) |
| `OGRE_OVERLAY=1`, `OGRE_OVERLAY_AT_MS=<n>` | open the overlay at startup / push one `ESC` after `n` ms |
| `OGRE_OVERLAY_TAB` / `_KEYS` / `_PAD` / `_OPACITY` | overlay tab (`start` = `main`); keydowns while visible (`main` + `Space` = EXIT GAME); pad presses always delivered (a first `back` opens it); opacity 0.2..1.0 (default 0.90) |
| `OGRE_CAPTURE_OVERLAY=<path>` | the overlay as a PPM once; `OGRE_OVERLAY_SHOT_MS` delays it |
| `OGRE_WINDOW_CLOSE_AT_MS=<n>` | one `SDL_WINDOWEVENT_CLOSE` for the game window |
| `OGRE_SPEED_KEYS=<up\|down>,…` / `OGRE_SPEED_PAD=<tag>,…` | synthetic `=`/`-` or R3 presses every 150 ms; `_AT_MS` (default 1000) |
| `OGRE_CAPTURE_BADGE=<path>` | the speed badge as a PPM on every show |

### Settings overrides

| Variable | Effect |
|---|---|
| `OGRE_SPEED=<n>` | GAME SPEED, 1..64. `OGRE_SPEED=8` reaches the attract loop's second variant in ~42 s instead of ~360 s |
| `OGRE_WIDESCREEN=off\|on` | WIDESCREEN; each transition prints `[widescreen]` |
| `OGRE_WS_SCENES=<hex>[,<hex>…]` | Expand scenes (default `0x03`, `0x0D`; HD-background scenes added) |
| `OGRE_RESOLUTION=native\|2x\|auto` | RESOLUTION |
| `OGRE_DISPLAY_EXPERIMENTS=1` | show and apply MSAA |
| `OGRE_MSAA=off\|2x\|4x\|8x` | MSAA (enables the row) |
| `OGRE_WINDOW=windowed\|fullscreen` | WINDOW |
| `OGRE_SOUNDS=off\|on`, `OGRE_VOLUME=<0..100>` | SOUNDS, VOLUME |
| `OGRE_MOUSE=off\|on` | MOUSE ENABLED |
| `OGRE_GRAPHICS_API=<auto\|d3d12\|vulkan>` | pin RT64's backend. RT64 switches D3D12 to Vulkan on an old driver (NVIDIA ≤ 475.14, AMD ≤ Jan 2019, Intel 6th-gen ≤ 31.0.101.2115); the boot log names the GPU and whether the fallback ran |

### Logs, traces and dumps

| Variable | Effect |
|---|---|
| `OGRE_SCENE_LOG=1` | one `[scene]` line per scene change, with descriptor and record mask |
| `OGRE_SCENE_TRACE=1` | the forcing state every 500 ms |
| `OGRE_RSP_LOG=1` | two `[rsp]` lines per RSP task (~58 tasks/s). `tools/runlog.py` needs them |
| `OGRE_DL_TRACE=1` | one `[renderer] display list N at t=…ms entries=<n>` line per list plus RT64's time for it |
| `OGRE_DEBUG_TRACES=1` | `[ev]`/`[mq]`/`[sch]`/`[vi-debug]`, and `[pi] inline DMA` per streamed load; `OGRE_DEBUG_VI=1` adds every `osViSetMode` and VI geometry change |
| `OGRE_HANG_MS=<ms>` | the hang watch threshold (default 5000; `0` = off): no game frame for that long prints `[hang]`, runs the `[snap]` snapshot once and writes `error.log`. See "Crash reports" |
| `OGRE_LOOKUP_CHECK=0` | turns off the check of streamed lookups against resident code (`[lookup]` lines). See "The lookup check" |
| `OGRE_SNAP=1\|<frames>` | the periodic message-queue snapshot `[snap]` (default every 90 VI frames), for a boot whose threads are all parked. It runs on the Critical-priority VI thread and sleeps 100 x 200 us; on Windows each sleep takes ~16 ms (the 15.6 ms timer), so one snapshot stalls the VI ~1.6 s |
| `OGRE_CHAIN_HISTORY=<tid>` | the sequence of live call chains thread `<tid>` goes through |
| `OGRE_SCHED_TRACE=1` | scheduler traces and the `[sched-ring]` event ring at exit; `_TID=<tid>` filters it |
| `OGRE_PROFILE=1` | the sampling profiler and shadow call chain. Its per-thread function is the most recently entered one: `recomp_trace_return` does not restore `last_func_vram` |
| `OGRE_COVER=1\|<path>` | at exit, window close or console `cover`, one `[cover] 0xADDR count` line per recompiled function entered; a path writes its own file. No sampling thread |
| `OGRE_DMA_TRACE=1` | at `OGRE_EXIT_AFTER_MS`, every streamed PI DMA grouped by `(rom, ram)` base in `0x80190000..0x801C0000`; `_FULL=1` all bases. Changes timing, and can change which bank loads |
| `OGRE_PROBE55=1` / `OGRE_PROBE57=1` | New Game sequence state on change: scene, `D_8018F1C0`, `D_8018F1C2`, plus script-buffer words (55) or the `0x0D` exit vtable `0x801D0830` and engine words `0x8023A994`/`0x8023A960`/`0x801DFC90` (57) |
| `OGRE_DUMP_RDRAM=<path>` | the 8 MiB host RDRAM image at exit and beside `error.log` on a crash. See "Reading a dump" |
| `OGRE_RAM_KEY=<1..9>` | that key writes `ram-<n>.bin` and `.txt` to `OGRE_RAM_KEY_OUT`, else `OGRE_PREF_DIR`, else `/tmp`. `_ADDR` (default `0x802696C8`), `_LEN` (`0x1000`), `_STRIDE` (summary slot size, `0x80`; marks empty slots), `_AT_MS` (one timed dump), `_FULL=1` (whole image, ROM order, `ram-<n>-full.bin`). `tools/ogre_ram.py` reads it (`find-ptr`, `cell`, `empty-cells`) |

### Renderer diagnostics

| Variable | Effect |
|---|---|
| `OGRE_DL_ANALYZE=1` / `OGRE_DL_DECODE=<n>\|all` | per-list F3DEX2 counts / the decoded command stream. Both decode every list as F3DEX2, so an S2DEX2 list reads wrong |
| `OGRE_TEX_TRACE=1` | one `[tex]` line per `G_TEXRECT` before RT64 parses: rectangle, `s`/`t`/`dsdx`/`dtdy`, `SETTILESIZE`, tile line and palette, `SETTIMG` |
| `OGRE_TEX_TRACE_KEY=<1..9>` / `_KEY2` / `_MS` / `_AFTER_MS` | arm on a key press, closed after `_MS` (default 2000; `0` = until close); a second key; a one-shot timer |
| `OGRE_TEX_TRACE_DUMP=<dir>` | also write the sampled RDRAM (`tex/<n>_src.bin`, `_pal.bin`, `.json`); `tools/ogre_tex_uv.py <dir>` summarises |
| `OGRE_TEX_TRACE_COLOUR=1` | decode each window and print `colour: mean N/255`, plus one `[tex] BLACK DRAW:` line for the first draw at or below `OGRE_TEX_TRACE_BLACK` (default 4/255, ≥ 32 opaque texels) |
| `OGRE_TEX_TRACE_DRAW` / `_MAX` / `_MIN_PX` / `_IMAGES` | only the nth rectangle / a cap / skip small rectangles / each bound image once with a nonzero-byte count |
| `OGRE_PRESENT_TRACE=1` | `[present]`/`[state]` per present |
| `OGRE_RDP_TRACE=1` / `=2` | `[rdp]` `setColorImage`/`fillRect`/`drawRect`; `=2` a `[cimg]` histogram and `[cimgseq]` list of colour-image targets |
| `OGRE_WORKLOAD_TRACE=1`, `OGRE_FBRENDER_TRACE=1`, `OGRE_VI_TRACE=1` | `[workload]` framebuffer pairs; `[fbrender]` per-call cycle/fill/rect; `[vi]` the decoded VI to present |
| `OGRE_RECT_STATE=<y0>-<y1>` | render state of rectangles inside those rows: cycle type, combiner cycles, blender, primitive colour, tile |
| `OGRE_TILE_TRACE=1` / `OGRE_TILE_TRACE_ALL=1` | `[gpu-tile]` for degenerate tiles / every tile (`_MAX`, `_MAXH=<n>`, `_MISS=1`, `_COPY=1`) |
| `OGRE_SPRITE_TRACE=1` | `[spr]` per tile of a two-texture draw after the upload decision: path taken, hash, `tileCopy`, `rawTMEM`, RDP tile |
| `OGRE_TILE_DECODE=1` | per hashed tile, the RGBA decoded from its TMEM snapshot (`TextureDecodeCS.hlsl` rules); `[fbcopy]`, `[fbcopy-stale]`, `[fbcopy-outside]`, `[fbcopy-nowrite]` for framebuffer tile copies and refusals |
| `OGRE_DUMP_TEX=<dir>` | RT64's decoded-texture dump (`.tmem` + `.tile.json`) |
| `OGRE_EMPTY_TILE_TRACE=1` | rectangles skipped because their tile covers no texels |
| `OGRE_S2D_TRACE=1` | `[s2d]` the first 40 S2DEX object commands with the decoded `uObjSprite` and destination |
| `OGRE_CONVERT_TRACE=1` | `[convert]` the raw 9-bit `G_SETCONVERT` fields |
| `OGRE_FB_WRITEBACK=1` | `[fbpair]` framebuffer pairs and `[fbwrite]` the RDP→RDRAM writeback |
| `OGRE_SYNC_TRACE=1` | `[syncfb]` the framebuffer manager at the CPU readback; `[njpair]` pairs for `0x400`/`0x25C00`/`0x4B400` |
| `OGRE_YUV_TRACE=1` | `[yuv]` per `G_SETTIMG` with `fmt=YUV` |

### Renderer behaviour

| Variable | Effect |
|---|---|
| `OGRE_RECT_LR_FIX=1` | restore RT64's "Fix LR with Scissor" (off here): it widened a rectangle one pixel inside the scissor, and the unit profile's last DEF/MDEF digit sampled the next font cell (session 101) |
| `OGRE_OVERSCAN=<0..8>` | crop off the framebuffer's right/bottom edge (default 1 px). OB64's scissor covers 319x239 of 320x240, and the last row and column flicker with leftovers. Framebuffers are sized from the VI (`framebufferHeightForDisplay`) |
| `OGRE_FB_GPU_COPY=0` | turn off RT64's `framebuffer.copyWithGPU`, so every texture load reads RDRAM |
| `OGRE_NOP_RECT=<selectors>` | replace matching `G_TEXRECT`s with `G_SPNOOP` (`any`, `tex:<hex>`, `flip`, `x:<a>-<b>`, `y:<a>-<b>`, `n:<index>`) |
| `OGRE_EMPTY_TILE=draw` | draw rectangles whose tile covers zero texels |
| `OGRE_FOG=0` / `=alternate` | disable / alternate the repair that makes OB64's fog "wrap" rectangles sample the layer image (layer table `D_801B80E0`); it draws the title fog and the `©1999 QUEST` sweep |
| `OGRE_FOG_SCALE=<percent>`, `OGRE_FOG_TRACE=1`, `OGRE_FOG_SUMMARY=1` | fog intensity (default 100 = the game's asset); per-group and per-list fog traces |
| `OGRE_PRESENT_ALWAYS=1` | present on every VI |
| `OGRE_PRESENT_FBTARGET=1` | with no framebuffer at the VI address, present a non-empty render target |
| `OGRE_INSTANT_PRESENT=1` | RT64 `PresentEarly` |

### Captures and the synthetic probe

| Variable | Effect |
|---|---|
| `OGRE_CAPTURE_PRESENT=<path>` | GPU-read the swap-chain texture at each present into `<path>.<n>.ppm` |
| `OGRE_CAPTURE_TARGET=<path>` | the render target the VI renderer sampled, `<path>.<n>.bin` (`R16G16B16A16_UNORM`) |
| `OGRE_CAPTURE_AFTER=<n>` / `OGRE_CAPTURE_EVERY=<n>` | skip the first `n` presents / capture every `n`th |
| `OGRE_SYNTH_FRAME=1` | submit a seven-bar F3DEX2 list through the task path; sets `OGRE_PRESENT_ALWAYS` and `OGRE_PRESENT_FBTARGET` unless set |
| `OGRE_SYNTH_AT_MS` / `_PERIOD` | first submit (keep above ~600 ms, before the game's ucode is in RDRAM) / VIs between submits (default 30) |
| `OGRE_SYNTH_RAW` / `_ANIMATE` / `_NO_DL` / `_FORCE_VI=0` | also write the bars into RDRAM / rotate their palette / skip the list / do not pin the VI to the probe |
| `OGRE_NO_DUMMY_VI=1` | skip the pre-start dummy VI workload |

### njpeg and HD backgrounds

| Variable | Effect |
|---|---|
| `OGRE_NJPEG=0` | stub the `M_NJPEGTASK` decoder (the recompiled microcode runs by default, 0-1 ms per image) |
| `OGRE_NJPEG_TRACE=1` | `[njpeg]` the colour image the YUV draw landed in (RT64's never-cleared scratch word) |
| `OGRE_NJREAD_LOG=1` | `[njread]` per stage-3 pass; see "`OGRE_NJREAD_LOG`" |
| `OGRE_BG=0`, `OGRE_BG_DIR=<dir>` | run without the `mods/backgrounds/` pack / use only that folder (`docs/guides/hd-backgrounds.md`) |
| `OGRE_BG_LOG=1`, `OGRE_BG_DEBUG=1` | `[hdbg]` per pass and chunk / paint the four passes in solid colours |
| `OGRE_BG_DUMP=<dir>` | write the game's 496x384 backdrop as `original-step<NNNN>.png` (`tools/backgrounds.py`, `make bg-extract`) |
| `OGRE_BG_HD=0` | keep the pack's game-resolution backdrop and skip its HD strips (`mods/backgrounds/hd/`, `make bg-hd`) |

## Scripted runs and captures

Scripted taps are for short, exact routes. A played state is reached from a save
(`OGRE_SAVE`, AGENTS.md rules 11 and 13).

```sh
OGRE_NO_AUDIO=1 OGRE_TAP_MS=5000 OGRE_EXIT_AFTER_MS=60000 "$APP" "$ROM" > C:/tmp/ogre.out 2>&1
grep -A 12 "per-thread last" C:/tmp/ogre.out
```

### Scene forcing: jumping straight into a screen

`OGRE_SCENE` names: `title` (0x04), `intro` (0x09), `publishers` (0x0A),
`story` (0x0B), `unit-info` (0x0C), `tutorial` (0x17), `menu` (0x18), `new-game`
(0x02), or a hex id. The app stores the game's own scene request,
`0x8000 | id` into `D_800C4C26`, once a scene after the boot's `0x00` runs with
no request pending (`0xFFFF`). The dispatcher `func_80075BC0` then runs the
running scene's leave hook and replaces it with the target (`0x800760A0`), as
in a natural transition. The request is repeated, from the per-frame poll and
the streamed-DMA hook, until `D_800E810E` reports the target. `OGRE_SCENE=title`
reaches `0x04` at about 1.25 s, from the intro `0x09`. The root id
`D_800E8214`, which the old poke wrote, is read only at the dispatcher's loop
top, so that poke had no effect. A forced scene enters without the pre-state a
natural path builds, so a forced scene that misbehaves is not by itself a
finding; forced `menu` and `new-game` were recorded as crashing.

```sh
OGRE_NO_AUDIO=1 OGRE_SCENE=title OGRE_SCENE_TRACE=1 OGRE_EXIT_AFTER_MS=6000 "$APP" "$ROM"
OGRE_NO_AUDIO=1 OGRE_SCENE=title OGRE_TAP_MS=2500 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,a,a,a,a,a,a,a" "$APP" "$ROM"   # New Game
```

### Capturing the presented frame

A desktop screenshot can return a stale frame; `OGRE_CAPTURE_PRESENT` reads the
swap-chain texture.

```sh
OGRE_NO_AUDIO=1 OGRE_SCENE=title OGRE_SPEED=4 OGRE_CAPTURE_PRESENT=C:/tmp/title \
  OGRE_CAPTURE_AFTER=1700 OGRE_CAPTURE_EVERY=10 OGRE_EXIT_AFTER_MS=26000 "$APP" "$ROM"
OGRE_NO_AUDIO=1 OGRE_SYNTH_FRAME=1 OGRE_SYNTH_AT_MS=900 OGRE_NO_DUMMY_VI=1 \
  OGRE_CAPTURE_PRESENT=C:/tmp/frame OGRE_CAPTURE_AFTER=200 OGRE_EXIT_AFTER_MS=8000 "$APP" "$ROM"
```

`OGRE_CAPTURE_TARGET` separates "the RDP did not draw" from "the presenter
dropped it". The port reads `OGRE_CAPTURE_PRESENT` once at startup and hands the
path to RT64 through `ogre_present_capture_path()`; the console's `snap` toggles
a flag the app owns, because the UCRT's `_putenv` copies the string (toggling
the environment left the capture on in v0.4.0 and crashed D3D12; session 107).
An environment value wins over `snap`. Without `OGRE_PRESENT_ALWAYS` a stalled
boot keeps an old frame; without `OGRE_PRESENT_FBTARGET` an RDP-only fill
presents empty RDRAM.

## Diagnostics toolkit

### Reading a dump

The runtime stores RDRAM with each 32-bit word byte-reversed: the logical word
at guest `a` is the little-endian word at host offset `a & 0x1FFFFFFF`, the
halfword is at `(a & 0x1FFFFFFE) ^ 2`, the byte at `(a & 0x1FFFFFFF) ^ 3`
(`console_word`/`console_half` in `sdl_platform.cpp`, `logical_byte` in
`ram_key.cpp`). Two dump layouts exist:

| writer | layout |
|---|---|
| `OGRE_DUMP_RDRAM` (`main.cpp`, `crash_log.cpp`), console `dump`/`snap` (`write_rdram_image`) | the host buffer: byte-reversed words |
| `OGRE_RAM_KEY_FULL=1` (`ram_key_dump_full`) | ROM order |

The tools accept both. `rdram.load_image` (used by `tools/rdram.py`,
`tools/ogre_ram.py`, `tools/ogre_frame.py`, `tools/refscan.py` and
`tools/scenemap.py --dump`) counts `jr $ra` (`0x03E00008`) in the main segment
(`0x80070C00..0x800AEDB0`): `03 E0 00 08` means ROM order, `08 00 E0 03` means the
host layout, which it byte-swaps per word on load. Every read is then ROM order
(word = `int.from_bytes(d[a & 0x1FFFFFFF : +4], 'big')`, byte =
`d[a & 0x1FFFFFFF]`), and `Dump.layout` says which file it was. Both layouts of
one run read `3C08800B` at `0x80070C00`, the ROM's word at `0x1000`.

```sh
tools/rdram.py <dump> word|half|byte|string|hexdump|find|ptr …
tools/rdram.py <dump> banks        # which module is resident in each streamed window
```

`banks` compares the image with every record in `app/src/bank_funcs.inc` plus
the uncompiled segment-table records and names the ROM offset live at each
streamed RAM base. Streamed RAM holds different modules at different times
(`0x802395E0` is `bankRec14b` for the movie visit, `bankRec14c` after), so a
wrong-bank call shows as another module's bytes at the faulting address.

### The live console

The console runs commands in the running game on the main thread. It is off
unless asked for, because `w` writes guest RAM and `load` restores a checkpoint.
`OGRE_LIVE_CONSOLE=1` turns on both triggers; `OGRE_CONSOLE_FILE` or
`OGRE_CONSOLE_AT_MS` turns on the file, `OGRE_KEY_<n>` the keys;
`OGRE_LIVE_CONSOLE=0` is an explicit off.

- A watched file (default `/tmp/ogre-console.txt`, i.e. `C:\tmp\`): its lines run
  and the file is removed. Write it atomically; a plain `> f` truncates first,
  and the app can read the empty file and delete it. `OGRE_CONSOLE_AT_MS=<n>`
  delays reads until `n` ms, so a script can leave the file in place at launch.
- Keys `1`..`9` run `OGRE_KEY_1`..`OGRE_KEY_9`, one command per press.

```sh
OGRE_NO_AUDIO=1 OGRE_LIVE_CONSOLE=1 OGRE_CONSOLE_FILE=C:/tmp/ogre-console.txt \
  "$APP" "$ROM" | tee C:/tmp/live.log
printf 'c\nr 0x8018FDAC 8\ndump C:/tmp/at-now.bin\n' > C:/tmp/oc.tmp \
  && mv C:/tmp/oc.tmp C:/tmp/ogre-console.txt
```

Output is `[console]` lines on stdout; addresses are guest `0x80xxxxxx`.

| command | what it does |
|---|---|
| `r <addr> [n]` / `rh` / `rb` / `rk` | logical words / halfwords / bytes (`addr ^ 3`) / raw host-order words |
| `d <addr> [len]` | hex and ASCII dump |
| `f <value> [max]` / `fb <hexbytes> [max]` | find a word / a byte pattern in RDRAM |
| `s <addr> [len] [max]` | strings in a range |
| `k <addr> [len]` | FNV-1a checksum, for A/B runs |
| `w <addr> <value>` | store a word (a real write) |
| `c` | scene, pending scene, descriptor and record mask, step, next, spin |
| `cover` | functions entered so far (needs `OGRE_COVER`) |
| `dump [path]` | the image at this instant (host layout); bare `dump` writes a new `/tmp/ogre-rdram-NNNN.bin` |
| `snap [prefix] [ms]` | the image with game threads parked (`<prefix>.bin`, bare `/tmp/ogre-snap-NNNN.bin`) plus RT64's frame capture for `ms` (default 400) to `/tmp/ogre-shot.<present>.ppm`. With `OGRE_VI_TRACE=1` the log names the framebuffer |
| `save [path]` / `load [path]` | write / restore a checkpoint (bare: `/tmp/ogre-checkpoint-NNNN.ckpt` / the last bare save) |
| `press <buttons> [polls] [x] [y]` | hold a pad press for `polls` input polls (hex); `<buttons>` is `+`-joined `OGRE_TAP_BUTTON` names, `x`/`y` the stick (`press none 60 -1 0` = full left). The game polls about once per VI, so a map-cursor step needs a few hundred polls |
| `help` | the list |

`OGRE_CONSOLE_ON_SCENE=<name|hex>`, `OGRE_CONSOLE_ON_STEP=<n>` and
`OGRE_CONSOLE_ON_CMD=<cmd>` run one command on the first frame that scene (and
step word `D_8018F1C0`) is active, once per process, with no other variable:

```sh
OGRE_NO_AUDIO=1 OGRE_CONSOLE_ON_SCENE=0x0D OGRE_CONSOLE_ON_STEP=974 \
  OGRE_CONSOLE_ON_CMD='save C:/tmp/ck.ckpt' "$APP" "$ROM"
```

### Checkpoints

A checkpoint rewinds the machine, so a route is played once. The file is
`[8-byte magic][u32 version][u32 rdram_size][u32 host_size][u32 flags][u64
fnv1a][overlay-state blob][RDRAM image]`. `load` checks magic, version, sizes,
checksum and the executable's FNV-1a build id, so a rebuild invalidates every
checkpoint.

- The blob carries the loaded section and bank records and rebuilds the
  runtime's `func_map` (`recomp::overlays::get_overlay_state_blob` /
  `restore_overlay_state_blob`); RDRAM alone would run a later bank's bodies.
- `save`/`load` park every thread running recompiled code at a function entry
  (`ultramodern::checkpoint_pause_begin()`/`_end()`, via `recomp_trace_entry`)
  during the I/O, so the image does not tear.

Limits: a checkpoint is valid only in the process that wrote it, because the
image holds host pointers (`OSThread::context`, one per N64 thread) and a `load`
in another process crashes in `do_send`; transferable checkpoints need the
per-thread context registry and scheduler state rebuilt after the rewind. The
app's knob state and RT64's render state are not rewound. Save inside a stable
step: a checkpoint taken at a scene/step transition (the `0x02` loader visit)
restores into the movie-mode branch and bounces between `0x00` and `0x0D`.
Check `c` before `save`.

### `tools/runlog.py` — one screen per run

```sh
OGRE_NO_AUDIO=1 OGRE_SPEED=4 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D OGRE_SCENE_LOG=1 \
  OGRE_RSP_LOG=1 OGRE_DL_TRACE=1 OGRE_EXIT_AFTER_MS=45000 "$APP" "$ROM" > C:/tmp/run.log 2>&1
tools/runlog.py C:/tmp/run.log              # scene timeline, RSP tasks, bank loads, problems
tools/runlog.py C:/tmp/run.log --check      # exit 1 on crash / stub call / unknown module
tools/runlog.py C:/tmp/run.log --json
```

Graphics tasks always reach RT64; a non-gfx task served by `stub microcode` is
work the port drops, and the task table lists each with its ucode, data and
`data_ptr`. `--check` fails a log with no task lines.

### `tools/dlgaps.py` — a repeated stutter

```sh
OGRE_NO_AUDIO=1 OGRE_DL_TRACE=1 OGRE_EXIT_AFTER_MS=40000 "$APP" "$ROM" > C:/tmp/run.log 2>&1
tools/dlgaps.py C:/tmp/run.log                 # gap distribution + spike phase histogram
tools/dlgaps.py C:/tmp/run.log --check         # exit 1 when the spikes are periodic
tools/dlgaps.py C:/tmp/run.log --period 3000
```

It skips the first 3 s (`--after`) and counts a gap above `--spike` x p50 as a
spike. Spikes less than three median frames apart merge into one stall event.
The period is the median spacing between events unless `--period` names one, so
a periodic stall that stretches its own wall-clock period stays in one phase
bin. `--check` fails when 5 or more events put 60% of themselves in one tenth of
the period, the signature of the `[snap]` snapshot and of a
`yield_work_loop_branches` misfire. Spread-out slow frames report
`not periodic`, so read the p90 and the spike count too.

### A slow screen

Read `OGRE_PROFILE=1`'s `t4` line as a lead (it is the most recently entered
function). `OGRE_DL_TRACE=1` gives the frame period series, and `entries` is a
list's cost in recompiled function entries. A host stack sample of the game
threads names the function each is in; a chain through `yield_self` means the
frame period is a recompiler-injected poll-loop wait. `debug/menu-probe.sh` is
built on macOS `sample` and has no Windows equivalent yet; per-thread CPU is
available through the thread names ("Windows setup" step 8).

The usual cause is a work loop whose backward branch `is_yield_poll_loop`
(`tools/N64Recomp/src/recompilation.cpp`) took for a poll: it terminates on a
counter or on data already present. The fix is an entry in the unit's
`yield_work_loop_branches` (`config/banks/config-bank<U>.toml`); current entries
are `0x801B3008`, `0x801DA2C0`, `0x801D0C08`, `0x8020A910`, `0x80204C48`.

### `make elf-rom-check`

`tools/elfcheck.py` compares every `CONTENTS` section of every linked ELF with
the ROM at the same offset, and `--syms` asserts every symbol named with eight
hex digits (`func_ovlM_801A2A7C`) is at that address. Run it (in WSL) when a
scene draws the wrong size, slice or string from a table. `mips-linux-gnu-as`
pads `.text` to 16 bytes, so a record whose code/data boundary is only 8-byte
aligned links its data 8 bytes high and every `%lo` data reference reads 8
bytes off (unit M, session 65). Fix it in the unit's config (`align`/`subalign`,
or end the `asm` subsegment on a 16-byte boundary). `make bank-recomp` runs the
check first.

### `tools/rdram.py` image and diff

```sh
tools/rdram.py C:/tmp/rdram.bin image 0x80243E28 --width 320 -o C:/tmp/bg.png
tools/rdram.py C:/tmp/rdram.bin image 0x80243E28 --fmt ia8 --width 320
tools/rdram.py diff C:/tmp/a.bin C:/tmp/b.bin --min-run 16 --limit 20
```

`image` renders a region as an N64 texture (`rgba16`, `rgba32`, `ia16`, `ia8`,
`ia4`, `i8`, `i4`, `ci8` + `--palette`) to a PNG with a luminance summary and a
`(near-)uniform` note, which separates "nobody drew this" from "an asset failed
to decode". `diff` clusters changed bytes into runs (`--gap` merges) and groups
them by owning record (`tools/n64map.py`), flagging shared RAM. Both read ROM
order.

### `OGRE_NJREAD_LOG`

One `[njread]` line per njpeg stage-3 pass (`func_ovlE_8019976C`): pass, scene,
step, the framebuffer the game's `state[0x64]` selects, the rule applied
(`game`/`scratch`), the scratch word, the display word `D_800C4BB8`, and the
destination and size. A New Game assembly runs four passes, the tiles of the 2x2
backdrop: 240x320 top-left `0x801AAE90`, 240x176 bottom-left `0x801D06B0`,
144x320 top-right `0x801E50D0`, 144x176 bottom-right `0x801FB8F0`. The correct
sources are the frames `573FF47B8A5A3279`, `48892A76C362A4BA`,
`3F79B6732153B2FD`, `38BCBEBE5B555C3C`; pass 0 is where a stale selection
shows. To check the result, dump while `c` reads `desc=0x8018FC3C`,
`step=0x021D8002` (the cathedral) and render `0x80243E28` at width 320. See
`docs/guides/njpeg-backgrounds.md`.

### `tools/guestmap.py`

```sh
tools/guestmap.py 0x801B7EBC 0x802395E0 0x2F180    # vram or rom, auto-detected
tools/guestmap.py 0x801B7EBC --context 4
tools/guestmap.py --list
```

Prints the ROM offset (`vram - 0x80070C60 + 0x1060`), the owning segment,
record and unit, whether the address is a function entry in the ELF that owns
that layout, and the other records mapping the same RAM. `0x801B7EBC` is
`func_801B7B9C + 0x320` in `ogrebattle64.elf` and an entry in `bankC.elf` and
`bankG.elf`; the game's DMA decides which is live.

### `tools/scenemap.py`

```sh
tools/scenemap.py                 # all three tables (~3 s, ROM only)
tools/scenemap.py scenes | transitions | steps
tools/scenemap.py scenes --dump C:/tmp/map3.bin   # descriptors from a live image
```

`func_80075BC0` writes 25 accessor pointers into `D_800AF028[0..24]`, each
returning a descriptor from static ROM data; 20 resolve statically, ids 2, 3, 6,
8, 23 print `(indirect accessor)`. Every store into `D_800C4C26` is 39 sites in
30 functions (21 static). The step table (asset `0x19A8804`, ROM `0x1F3CA54`)
holds 1693 per-step asset ids; the dialogues are entries in it.

### `tools/arenamap.py`

```sh
tools/arenamap.py                      # every arena and bank (~10 s, offline)
tools/arenamap.py --missing | --verify | --coverage | --entries | --md
tools/arenamap.py --arena 0x80214FA0
```

Every bank DMA is `lui/addiu x3 -> jal func_8009DA50 -> subu a2,a2,a0` (the
size) followed by `func_800900C0(ram_base, code_size)`,
`func_80090010(code_end, data_size)` and `func_80093380(data_end, bss_size)`, so
each block yields ROM start, RAM base, size and the code/data split. A bank's
size is the loader's `subu` (the assembler sometimes hoists it above the `jal`),
never the chunk count. `--verify` checks every unit against its loader;
`--coverage` lists uncovered ROM gaps and any `jal 0x8009DA50` in them. Last
answer: 27 bank loads in 5 arena windows, 26 compiled; the gap is a `0xE80`
scene-`0x14` setup fragment whose RAM another unit owns.

### `tools/stubmap.py`

```sh
tools/stubmap.py report [--verbose] [--json] [--strict]
tools/stubmap.py target 0x80219594      # owners, bodies, callers, scenes
tools/stubmap.py log C:/tmp/run.log     # attribute a run's stub hits
tools/stubmap.py pointers               # data words pointing at risky targets (jalr)
```

It reads the registration tables (`app/src/bank_funcs.inc`,
`RecompiledFuncs/recomp_overlays.inl`) and the generated C's `jal` comments.
Buckets: DEFINITE (no module registers the address), MISSING FUNCTION (a
resident-capable bank lacks the entry and its bytes start a function; act on
these first), UNRESOLVABLE (no compiled code covers it), BANK SELECTION (a body
interior in the missing bank), INERT (never co-resident with the caller), RSP
IMEM. A MISSING FUNCTION needs the code before the address to end in `jr ra`
plus its delay slot, no branch in the containing body to reach the address, no
word of the module to hold the address (a jump-table case), and a frame or a
leaf return at the address. The old test also accepted `lui`, `sw`, `j`, `jal`
and `jr` as openers; its 131 candidates were mid-function code, switch cases
and data. Last measurement (2026-10-31): 988 static targets, 0 definite, 0
missing-function, 0 unresolvable, 284 bank-selection, 369 inert, 332
resolvable, 3 IMEM; 288 `jalr` sites. The runtime's lookup check (below)
verifies the bank-selection targets and the `jalr` sites as they run.
`log` matches the live words the runtime prints at a stub
(`[overlays] @0xADDR: …`) against every module's ROM image, names the resident
module, and says whether its load bypassed `on_streamed_dma` or the caller
dispatched an address the bank does not have; bytes that match nothing compiled
are a bank the port has no record for, and the tool names the ROM record.

### The lookup check — `[lookup]` lines

`get_function` asks `lookup_check` (`app/src/bank_overlays.cpp`) about every
address in `0x8016A000..0x80400000`, which covers every `LOOKUP_FUNC` and every
indirect `jalr` into streamed RAM. It compares the resident words with the
mapped body's ROM words, from the entry to the delay slot of the first `jr ra`
(at most 256 bytes). On a mismatch, or on a miss, it calls the body from any
bank record or main-ELF section whose ROM words are the resident ones, and logs
once per address:

```
[lookup] 0x8019A884: the map has the body at rom 0x…; the resident code is rom 0x…, so that body runs
[lookup] 0x8019A884: not in the map; the resident code is rom 0x…, so that body runs
[lookup] 0x8019A884: the resident code matches no compiled body; the map's body (rom 0x…) runs
```

The first two are a bank load `on_streamed_dma` did not recognise; the third is
a module the port has not compiled. `tools/guestmap.py` names the unit of a ROM
offset. A verdict is cached per address until the next DMA into the window; a
30 s run costs the same CPU with and without it. `OGRE_LOOKUP_CHECK=0` turns it
off. Direct calls bypass it: `make cross-bank-check` lists the main unit's
direct calls into another section's swappable RAM (30 on 2026-10-31, each bound
to the only module `stubmap.py target` finds resident with the caller).

### `tools/recompcov.py`

```sh
tools/recompcov.py [--no-scan] [--json] [--log cover.txt]
```

| measure | last value |
|---|---|
| code bytes | 99.05% of `0x001000..0x2B8B70`; the one gap (`0x0DDF80..0x0E4910`) is data |
| modules | 28/28 loader-pattern arenas have a unit; 44 bank records in 34 units + 10 base sections |
| dispatch | 985 static targets, 0 unresolvable; 288 `jalr` sites |
| execution | per run, from `OGRE_COVER` |

Measure execution from a session played by hand
(`OGRE_NO_AUDIO=1 OGRE_COVER=C:/tmp/cover.txt "$APP" "$ROM"`, close the window,
copy the file to WSL). Modules at 0% are the untested surface.

### Adding a streamed bank unit

1. Read the port's report: `[bank] UNKNOWN module rom=0x… ram=0x…` or
   `[overlays] streamed function stub called @ 0x…`.
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
   to the no-op stub. Derive the list from the `LOOKUP_FUNC(0x…)` targets in
   `Bank<U>Funcs/` inside the unit's RAM that are not a `recomp_trace_entry`.
5. `OGRE_DMA_TRACE` changes timing; a one-line probe in `recomp::do_rom_read` is
   lighter for finding a bank.
6. If a call runs the wrong bank's body, move the target record to a unit that
   does not call it, so the caller compiles as `LOOKUP_FUNC`.
   `make cross-bank-check` is the audit; `tools/cross_bank_known_hazards.txt`
   lists accepted hazards, and a new one fails `make bank-recomp`.

Then regenerate in WSL and copy the outputs ("Windows setup" step 3).

### `tools/watch.sh`

`tools/watch.sh <guest-addr> [--after SYMBOL] [--ignore N] [--hits N] [--size N]
[--value V] [--read]` arms an lldb watchpoint on `rdram + (guest - 0x80000000)`
after a guest symbol runs and prints a backtrace per hit. It has been used on
macOS only. On Windows, set a debugger data breakpoint on the same host address:
the crash handler prints the RDRAM base.

## Troubleshooting

- **RT64 submodules do not check out** ("Unable to find current revision"):
  `git -C tools/RT64/src/contrib/imgui fetch origin <sha>`, then `checkout <sha>`.
- **A patch does not apply** (`\ No newline at end of file`): add
  `--ignore-whitespace`.
- **`cl : command line error D8021`**: `build-dist/` was configured without the
  clang-cl `CMAKE_ARGS`. Delete it and rerun `make app` with them.
- **The exe exits at once from `build-dist/`**: copy `dxcompiler.dll` and
  `dxil.dll` beside it from `build-dist/RT64/`.
- **`tools/smoke-dist.sh` fails in `pe_imports.py`**: `python3` is the Store
  stub.
- **A script runs in WSL**: call it with `/usr/bin/bash`.
- **A console command never runs**: the file went to Git Bash's `/tmp`
  (`%TEMP%`) while the app polls `C:\tmp\`, or it was written non-atomically.
- **RT64 build still fails after changes**: delete `build-dist/` and rebuild.
