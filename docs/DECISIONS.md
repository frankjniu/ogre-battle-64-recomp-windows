# Durable decisions

This file lists the decisions that still bind the Windows port of Ogre Battle 64. Each entry states
the current rule, with later corrections folded in, and ends with the session that made it. The full
per-session log, with evidence, run IDs and superseded entries, is this file's history in git
(`git log -p docs/DECISIONS.md`); the old session handoffs are in git history too.

When a decision changes, edit its entry here. Findings that do not constrain future work stay out of
this file.

## Scope

- **The port targets Windows only.** The browser, macOS and Linux builds are not built or verified
  unless the developer asks, and open work is ranked by its effect on the Windows build. A source
  shared with the browser build still guards native-only includes with `#ifndef __EMSCRIPTEN__`
  (sessions 109, 138, 145).
- **The app is GPL-3.0.** N64ModernRuntime is GPL-3.0 and statically linked; N64Recomp and RT64 are
  MIT (Phase 3, 2026-08-24).

## Build and code generation

- **`make regenerate` is the generation pipeline, in a fixed order: `resplit` → link the ELF →
  `bank-recomp` → `recomp` → `rsp-recomp`.** `make recomp` dispatches cross-bank calls against the
  records `make bank-recomp` writes to `app/src/bank_funcs.inc`; without them 15 sites stay bound to
  the wrong bank and the build still succeeds, so `recomp-prep` fails without the file (session
  87c).
- **Generated code is never committed.** `RecompiledFuncs/`, the 34 `Bank*Funcs/`, `RspFuncs/` and
  `app/src/bank_funcs.inc` are a translation of the ROM. On the Windows machine `make regenerate`
  runs in a WSL clone and the outputs are copied into the checkout; `docs/guides/app-build.md` lists
  the Windows build traps (sessions 87b, 145).
- **Build configs live under `config/` (`banks/`, `symbols/`), and upstream patches under
  `patches/`.** splat, N64Recomp and RSPRecomp resolve a config's paths against its own directory
  (session 97).
- **Clear a unit's generated outputs before regenerating it.** splat and N64Recomp never delete a
  previous run's files, so a stale file leaves a conflicting definition. A `data` subsegment needs
  an explicit `bin` gap after it (session 58b).
- **Every linked ELF must be byte-identical to the ROM, and a misalignment is fixed in the config.**
  `make elf-rom-check` (`tools/elfcheck.py`) runs inside `make bank-recomp`. Unit M's `.data` once
  linked 8 bytes high because `as` pads `.text` to 16 bytes, which shifted every data table by one
  entry (session 65).
- **`make resplit` always leaves `asm/1CE040.s` and `asm/40E80.s` dirty** (sessions 7, 87c).
- **`make bank-recomp` re-applies `tools/njpeg_readback.py` and `tools/hd_backgrounds.py` to unit
  E's njpeg readback copy,** so both edits survive regeneration (sessions 57, 114).

## Recompiler (N64Recomp)

- **libultra is bridged by name.** OB64's libultra functions are named in
  `config/symbols/symbol_addrs*.txt`, and `reimplemented_funcs` binds them to the runtime's
  `osXxx_recomp`. `func_80093810` is `osSendMesg`; `0x800935A0` is `osJamMesg` (sessions 2-4, 32).
- **`func_80089A10` (a yielding wait) and `func_8008BC40` (a synchronous ROM read through
  `recomp::do_rom_read`) are reimplemented in the runtime.** A plain `memcpy` of ROM bytes gives
  byte-swapped pointers (session 6).
- **N64Recomp emits fall-through tail calls for KMC's merged epilogues and processes static
  functions to a fixpoint.** `make midfunc` lists the prologue-less tails and the frame slots and
  registers each one reads (session 9).
- **A `function_sizes` override in `config/config.toml` ends on the function's own last instruction
  (its `jr ra` delay slot) and never runs past the next real function.** A `jal` into a
  size-extended body compiles as a call to the containing body plus an early `return`, which leaks
  the caller's frame; on the frame-pump thread `D_800AEFA4` freezes while `D_800C4BCC` keeps
  counting (sessions 31, 60, 86).
- **The poll-loop heuristic stays general, and a misclassified work loop is fixed per site in
  `yield_work_loop_branches`.** The injected `yield_self` blocks until the next external message,
  about one VI retrace per iteration, so it belongs only in a true poll loop.
  `config/banks/config-bankN.toml` lists `0x801B3008`, `0x801DA2C0`, `0x801D0C08`, `0x8020A910` and
  `0x80204C48`; three broad heuristic changes deadlocked the boot (sessions 8, 29, 80, 115).
- **Host behaviour inside a game function goes in a `[[patches.hook]]` in `config/config.toml`.**
  The first hook calls `ogre_idle_wait` (`app/src/overlays.cpp`) at `0x8007F998`, so the idle thread
  `func_8007F8E4` sleeps about 1 ms per pass instead of holding a host core. `yield_self` there put
  the idle thread into the running queue and froze one boot in four (session 145).
- **The trace hooks (`recomp_trace_entry`/`recomp_trace_return`) stay;** removing all 15,961 calls
  did not change a 30 s boot's CPU time (sessions 26, 145).

## Banks and cross-bank calls

- **Streamed records compile as bank units partitioned by RAM, and the game's PI DMA drives the
  swap.** The 34 units come from `config/banks/config-bank<U>.{yaml,toml}` and register in
  `app/src/bank_overlays.cpp`. `notify_rom_read` loads the bank whose ROM range and rom→ram delta
  match the DMA. A bank's extent is its own DMA size (sessions 33, 59).
- **A swappable RAM range is never compiled into a unit whose code calls into it.** Each bank of a
  shared range gets its own unit, so N64Recomp emits `LOOKUP_FUNC` and the runtime picks the
  resident module. `make bank-recomp` runs `tools/cross_bank.py check-banks`, which fails on any
  hazard outside `tools/cross_bank_known_hazards.txt` (sessions 45, 55, 59, 67b).
- **Every `jal` from the main ELF into RAM a streamed module occupies is dispatched at run time.**
  `make recomp` runs `tools/cross_bank.py dispatch --only <list>` (18 targets, the latest
  `0x801AB740` and `0x801AB76C`), which also repairs the tail-call emission so the caller continues.
  A new wrong-bank call is fixed by adding its target to that list (sessions 37, 60, 88, 145).
- **Every module the game can stream needs a unit and a registered record.** An unknown record never
  evicts the previous bank, so `LOOKUP_FUNC` silently runs the old bank's body. `tools/arenamap.py`
  derives the loader-pattern banks from the ROM; table-driven loads are found by diffing an
  `OGRE_DMA_TRACE=1 OGRE_DMA_TRACE_FULL=1` census against the records (sessions 72-73b, 85).
- **A bank load erases only the function-map entries it covers and zeroes the record's BSS from the
  segment table** (sessions 34, 38).
- **`get_function` returns a logging stub for an unknown streamed address.** The log lines
  `[overlays] streamed function stub called @` and `[bank] UNKNOWN module` name a missing module;
  `tools/stubmap.py` answers it statically (sessions 6, 68, 83).
- **A streamed lookup runs the body whose ROM bytes are resident.** `lookup_check`
  (`app/src/bank_overlays.cpp`, hooked into `get_function`) compares the resident words with the
  mapped body's ROM words up to its first `jr ra`. On a mismatch or a miss it calls the body, from
  any bank record or main-ELF section, that matches, and logs `[lookup]` once per address; with no
  match the map's answer stands. This executes the code the CPU would execute, so it is not
  fabricated state (AGENTS.md rule 7). It covers dispatched calls and indirect `jalr`, and not
  direct calls (2026-10-31).
- **The main unit's direct calls into swappable RAM are bound correctly as of 2026-10-31.** 919
  stay inside their own streamed section; the other 30 (`.streamedB` into `.streamedC`'s range)
  target entries of `.streamedC`, and `stubmap.py target` finds every other module at those
  addresses never loaded with the caller's record. A new one is checked the same way, and is
  dispatched with `cross_bank.py dispatch --only` if another module can be resident.

## Runtime (N64ModernRuntime)

- **Runtime changes live in `patches/n64modernruntime-ob64.patch`.** Regenerate it with the nested
  N64Recomp excluded: `git -C tools/N64ModernRuntime diff HEAD -- . ':(exclude)N64Recomp'`.
- **The scheduler uses a strict handoff token (`running`) and a separate idle poke (`poke`), and a
  drainer thread at priority 5 delivers external messages.** Higher priority numbers run first, so
  the drainer sits below the threads it wakes (sessions 4, 28).
- **A host bridge that converts a guest pointer sign-extends it first.** The `MEM_*` macros subtract
  `0xFFFFFFFF80000000`, so a zero-extended address lands 4 GiB past RDRAM (sessions 3, 91).
- **`recomp_mem_addr` maps the KUSEG low window (`a < 0x80000000`) to `a & 0x003FFFFF`.** It was
  added for stores that a wrong-bank call produced, and no current path is known to need it; A/B it
  before relying on it (sessions 44, 45).
- **`osViSetMode` translates its argument only for a KSEG0 address, and `start_game` runs from a
  thread delayed 500 ms.** Without the delay the VI thread crashes on a null mode (session 10).
- **`osAiGetStatus` reports busy while more than 52 ms of audio is queued
  (`ultramodern::audio_dma_busy()`).** The game's audio thread is paced by `AI_STATUS_BUSY`; a
  smaller cushion underruns and a constant 0 builds latency (session 76).
- **GAME SPEED scales the CPU counter and the VI schedule through
  `ultramodern::set_speed_multiplier()`.** The clock is piecewise linear with seqlock-published
  anchors, so `osGetTime` and pending timers stay continuous across a change (session 93).
- **The periodic `[snap]` queue snapshot is opt-in (`OGRE_SNAP`).** Each dump costs about 1.6 s on
  Windows (100 sleeps at the 15.6 ms timer resolution) (sessions 116, 145). A stalled frame
  counter runs it once and writes `error.log` (`app/src/hang_watch.cpp`, `OGRE_HANG_MS`).
- **The audio task's reply on `0x800C49E8` comes only from the game's scheduler.**
  `func_80089200` sends it after the RSP task completes (`0x80089324`). The runtime's bring-up dummy
  reply was removed (2026-10-31): it left a spare message in the 1-slot queue, so the game-flow
  thread ran one audio task ahead of the RSP while every task reuses the command list at
  `0x80136110`.
- **Closing the window is a hard exit.** The runtime never joins the game threads, so RDRAM is freed
  only when the run did not exit through `ultramodern::quit()`, and `main.cpp` ends with
  `crash_log::flush()` and `_Exit` (session 94b).
- **Non-gfx RSP microcode runs recompiled (`make rsp-recomp` → `RspFuncs/`), dispatched by ucode
  address in `app/src/rsp.cpp`.** The njpeg decoder and the audio microcode run by default;
  `OGRE_NJPEG=0` and `OGRE_AUDIO_UCODE=0` force the stubs (sessions 46, 47, 74, 75).
- **RSPRecomp's `text_address` is the IMEM address the microcode was assembled for, masked
  `0x1FFF`.** Both microcodes use `0x1080` (sessions 47, 75).

## Renderer (RT64)

- **RT64 interprets the gfx display lists with its auto-detected GBI.** The game's lists are F3DEX2
  2.08 and the njpeg lists are S2DEX2 2.08. RT64 changes live in `patches/rt64-ob64.patch` and
  `patches/rt64-plume-ob64.patch` (sessions 5, 10, 51).
- **RT64 implements S2DEX2's `0xDC` (`G_OBJ_MOVEMEM`) and `0xDA` (`G_OBJ_RECTANGLE_R`), and loads a
  YUV16 tile as the RDP's two-plane format** from the decoder's `[U, Y(even), V, Y(odd)]` words
  (sessions 50, 51).
- **YUV→RGB uses the game's `G_SETCONVERT` matrix: each 9-bit field is sign-extended and scaled
  `2*K+1`, and `K1` pairs with U.** The rows are `R = Y + K0*V'`, `G = Y + K1*U' + K2*V'`,
  `B = Y + K3*U'` (session 52).
- **The njpeg readback copies from the game's own framebuffer choice `state[0x64]`.** RT64's scratch
  word is stale at the first pass and is only a fallback. The copy calls `ogre_sync_framebuffers()`,
  and the game's DP-completion wait already orders it after the render, so no extra wait is added
  (sessions 57, 77).
- **`State::syncFramebuffers` reads each framebuffer's last written native slot.** The old call
  consumed slots of untouched framebuffers and dereferenced null (session 53).
- **A framebuffer tile copy is served only when the source framebuffer was written in the current
  workload and the tile lies inside its `lastWriteRect`; otherwise the load reads RDRAM.** Stale and
  depth framebuffer records drew sprite parts as flat ENV colour (sessions 134, 136). The writers of
  the two leftover records are not named, and the guard does not need them: no instruction in any
  ELF builds `0x32AF40` or `0x2E6840` (no `lui` of `0x8025..0x803F` or `0xA025..0xA03F`, no low half
  `0xAF40`, neither word in the ROM), so both are computed at run time; `0x32AF40` is
  `0x2E6840 + 0x44700`, row 219 of a 640-wide 16-bit image at `0x2E6840`.
- **The CRT filters run in RT64's VI pass from a box-filtered copy of the frame, and only
  permissively licensed shaders are used.** `VIRenderer::prepare` averages the frame to one texel per
  framebuffer pixel with `BoxFilterCS`, then `VideoInterfaceCrtPS.hlsl` draws crt-hyllian-fast (MIT),
  crt-lottes-fast or crt-lottes (public domain). GPL shaders (zfast-crt, crt-easymode, crt-geom) would
  put the executable under the GPL; the MIT notice is in `packaging/README-dist.txt` (2026-10-31).
- **RT64's "Fix LR with Scissor" (`enhancementConfig.rect.fixRectLR`) is off.** It widened the
  Organize Screen's font rectangles by a column; it is set after `Application::setup`, and
  `OGRE_RECT_LR_FIX=1` restores it (session 101).
- **The title fog repair (`OGRE_FOG`, on by default) gives the fog rectangles the layer image from
  the game's table at `D_801B80E0`.** The game leaves a zero-texel tile behind, which RT64 drew as
  an opaque white band (sessions 35, 36).
- **RT64's present and workload queue producers wait on a condition variable (`barrierCondition`).**
  Polling held 96 % of a core at GAME SPEED 8 (session 145).
- **The capture path comes from `ogre_present_capture_path()`, and plume's D3D12 `copyTextureRegion`
  skips `setSamplePositions` for a buffer destination.** The UCRT's `_putenv` copies its argument,
  so an environment toggle never hid the capture; this was the v0.4.0 crash (session 107).
- **`OGRE_GRAPHICS_API=auto|d3d12|vulkan` pins RT64's backend at renderer creation.** RT64 falls
  back from D3D12 to Vulkan on old drivers (session 94e).

## App, settings and UI

- **Player settings live in `<config>/settings.cfg`, and an `OGRE_*` variable overrides a key for
  one run without writing it back.** The keys are `game_speed`, `widescreen`, `sounds`, `volume`,
  `render_resolution`, `msaa`, `window` and `mouse` (sessions 93, 102, 121).
- **RESOLUTION defaults to `auto` (RT64 `WindowIntegerScale`) under the key `render_resolution`.**
  Files written while the row was hidden carry a `resolution = native` no player chose, so that key
  is ignored. Integer scales keep 2D pixels even (session 145).
- **MSAA stays behind `OGRE_DISPLAY_EXPERIMENTS=1` (or a set `OGRE_MSAA`).** It produced artifacts
  in session 121 and is unverified since; the configured value is kept and not applied.
- **The game window opens 1080 points tall (1440x1080; 1920x1080 with WIDESCREEN), and the process
  is per-monitor DPI aware (`SDL_HINT_WINDOWS_DPI_SCALING=1`).** Window sizes are in points and the
  drawable in pixels; the size is clamped to the display's usable bounds (session 145).
- **A fullscreen window's geometry belongs to RT64.** RT64 uses a raw `SetWindowPos` that SDL does
  not see, so `widescreen_fit_window` does nothing in fullscreen and leaving it calls
  `widescreen_request_refit()` (session 121).
- **WIDESCREEN `on` opens a 16:9 window and expands only the mission scene `0x03` and scenes an HD
  background pack covers.** The window shape never changes with the scene, and other scenes are
  pillarboxed; `app/src/widescreen.cpp` owns RT64's `ar_option` (sessions 105, 114).
- **GAME SPEED offers 1, 2, 4 and 8.** `=`/`-` step it, the pad's R3 cycles it, both are ignored
  while the ESC panel is open, and a badge window shows the multiplier for 2 s. Speed 6 stays out
  because the cursors are too fast to aim (sessions 93, 137).
- **The launcher and the ESC overlay share one tabbed panel (`app/src/ui.cpp`).** The overlay is a
  borderless always-on-top SDL window, because RT64 owns the swap chain. The game keeps running with
  an idle pad while it is open; every tab except MODS works there, and a pad's `Back` opens it only
  when no binding uses that button (sessions 92, 104, 121).
- **The controller map is player data in `<config>/controls.cfg`, and `ESC` is never bindable.** The
  C-button masks are libultra's `CONT_C_UP/DOWN/LEFT/RIGHT` = `0x0008/0x0004/0x0002/0x0001`
  (sessions 92, 99).
- **Every gamepad and the keyboard feed N64 controller slot 0.** The game reads only `OSContPad[0]`
  (`func_8008A600`) (session 118).
- **Mouse support is the MOUSE ENABLED setting, off by default.** It gates the world-map and
  mission-field cursors, the click as `A`, the edge scroll and the hidden pointer; the edge scroll
  injects the game's own D-pad bits (sessions 128, 129).
- **A mute is a gain of 0 in the audio callback, and the buffer is still queued.**
  `audio_dma_busy()` paces the game from the queue depth (session 106).
- **The DEBUG tab's Chaos Frame is `0x801936C9` (u8)** (session 95).
- **The live debug console is off unless the run sets `OGRE_LIVE_CONSOLE=1` or a trigger's own
  variable,** because its commands write guest RAM (session 117).
- **A stock run logs boot lines and errors only.** `[rsp]` lines need `OGRE_RSP_LOG=1` and
  per-display-list lines need `OGRE_DL_TRACE=1`; `tools/runlog.py --check` fails on a log without
  task lines (sessions 117, 145).
- **The ROM is resolved from the argument, `OGRE_ROM`, a ROM beside the executable, the stored copy,
  `assets/`, then the start screen.** The config directory is the executable's folder, so `saves/`,
  `mods/`, the `.cfg` files and `error.log` travel with the package (session 87).

## Windows platform

- **The `.exe` is a GUI-subsystem binary with a `WinMain` forwarder.** A double-click shows no
  console, a terminal launch attaches to the parent console, and `OGRE_CONSOLE=1` allocates one
  (sessions 94, 94d).
- **A standard stream keeps a redirect it inherited, and a stream with no descriptor is reopened on
  `NUL` on its own fd.** `main.cpp` checks `GetFileType` of fds 1/2 before `AttachConsole`, so
  `> run.log` and an Explorer launch's `error.log` both capture output (sessions 119, 145).
- **`crash_log` mirrors fds 1 and 2 into rings and writes `<config>/error.log` on a fatal event.**
  The Windows handler uses no stdio, and `setvbuf` gets a real buffer because the MSVC CRT
  fast-fails on a null one (sessions 94, 94c, 94d).
- **The Windows build uses clang-cl under Ninja with the static CRT
  (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`).** `cl.exe` rejects the code's `-W` flags.
  `dxcompiler.dll`, `dxil.dll` and their VC runtime DLLs ship beside the `.exe`. Directories are
  made with `std::filesystem::create_directories`, and `SDL_AUDIODRIVER` defaults to `wasapi`
  (sessions 89e, 94c, 94d, 135).
- **Patch files are LF everywhere (`.gitattributes`: `*.patch text eol=lf`).** A CRLF checkout
  breaks `git apply` on a hunk with no final newline (sessions 89b, 145).
- **Windows Defender quarantines the binary after a scripted NULL-handle launch or a fault-injection
  stub.** Use an Explorer launch for that state, or add a folder exclusion first (session 119).

## Saves

- **OB64 saves to 32 KiB of battery-backed SRAM (`recomp::SaveType::Sram`).** The PI device is
  chosen by the `OSIoMesg`'s `OSPiHandle`, and the runtime writes `<config>/saves/<game id>.bin`
  (sessions 65, 66).
- **A battery slot's header checksums are seeded with the slot's device offset
  (`0x10 + n*0x1850`),** so data is valid only in its own slot and a Controller Pak note (seed 0)
  cannot load into a slot directly. `tools/sramsave.py` imports emulator saves (sessions 66, 78).
- **The Controller Pak is the copy/backup device, bridged at three SI functions.**
  `__osContRamRead`, `__osContRamWrite` and `__osPfsGetStatus` are reimplemented in
  `librecomp/src/pak.cpp` over a flat 32 KiB image under the game's own PFS. The copy menu (scene
  `0x18`) renders, and its copy and erase flow is out of scope (sessions 65, 88, 88b).
- **A checkpoint (console `save`/`load`) is RDRAM plus the overlay state, written with the game
  threads parked, and valid only in the process that wrote it.** Native thread and message state is
  not captured, so hand a battery save between sessions (sessions 58, 65, 115).

## Mods

- **The mod system is on (`MOD_GAME_ID = "ogrebattle64"`, mods in `<config>/mods/`), and the start
  screen shows whenever a mod is installed.** Its runtime fixes are in `n64modernruntime-ob64.patch`
  (session 91).
- **A code mod hooks only functions in the base sections (`.entry`, `.main`, `.streamedA/B/C`).**
  `get_vrom_to_section_map()` comes from the base ELF, so a mod that needs bank behaviour hooks the
  frame function `func_80072944` and works on RAM (sessions 91, 103, 122).
- **Example mods ship disabled (`enabled_by_default = false`, a field `n64recomp-ob64.patch` adds to
  RecompModTool).** `skip-boot-logos` skips from scene `0x09`'s update, because skipping from `0x0A`
  leaves the title without display lists; `exp-overflow`; `item-randomizer` is experimental and has
  no options (sessions 91, 103, 125).
- **The Item Randomizer rerolls a slot that was empty and has just been filled, in both owned-item
  lists (`0x80196B20` and `0x80193AE0`).** A frame that fills more than one record is a bulk grant
  and is left alone, and the `+0x02` used count is recomputed from a reimplementation of
  `func_8016B774` (sessions 122-126).
- **The runtime accepts a stored mod config without `mod_id`** (session 123).
- **A background pack (`mods/backgrounds/NN.png`) replaces a whole njpeg backdrop at the readback.**
  `app/src/hd_backgrounds.cpp` matches on the step `D_8018F1C0`, scales the PNG to the 496x384
  canvas and splits it across the four passes (session 114). The bottom chunks start at canvas row
  239 and the right chunks at column 320: that is where the game draws them, measured with a
  row- and column-coded pack (2026-10-31).
- **HD detail comes from an RT64 texture pack keyed by strip hash, generated from the same PNG.**
  The game draws the backdrop as 116 `G_LOADTILE` strips (320x6, 176x11) per frame;
  `tools/backgrounds.py hd <id>` reads each strip's hash and rows from an `OGRE_DUMP_TEX` run with the
  pack applied, cuts the matching rectangle of the PNG at an integer scale, and writes
  `mods/backgrounds/hd/`, which the renderer loads with `loadReplacementDirectory`. Strips identical
  at game resolution share one hash and one HD image (2026-10-31).

## Releases and packaging

- **A release is prepared with `make release` (`tools/release.sh`).** It regenerates, proves the
  packed bundle equals the tree's generated code, commits it to the private data repository and
  dispatches `release.yml` with `draft` defaulting to `true`; `make release-check` is the dry run
  (sessions 111, 120).
- **Hosted runners build from a private bundle (`files.tar.gz`), named only by the `OGRE_DATA_REPO`
  variable and the `OGRE_DATA_TOKEN` secret.** The unpack step fails when the bundle's recorded
  commit differs from the one being built; it carries `build/mods/*.nrm` (sessions 89, 108, 111).
- **Each tracked patch must reproduce its vendored tree (`make patch-check`,
  `tools/patchcheck.py`).** The check covers files a patch creates and the union of every patch on a
  tree; `tools/rt64-plume-sdl.patch` counts for coverage only (sessions 117, 120).
- **`tools/smoke-dist.sh` (`make smoke`) gates every package.** It checks PE imports, launches with
  `OGRE_SMOKE=1`, requires an `OGRE_CRASH_TEST=segv` `error.log` that holds the `[boot]` line, and
  fails on an empty log or a `mods/` with no `.nrm`. On a Windows host with an Explorer shell it also
  launches the exe through `explorer.exe` and a temporary `.lnk` that passes `--crash-test=segv`
  (a launch with no standard handles; Explorer passes its own environment, not the script's) and
  requires the same `[boot]` line in that report. The workflow's `platforms=windows` input builds
  Windows alone (sessions 94c, 94d, 108, 145).

## Diagnostics and process

- **Toolkit diagnostics stay in the build behind environment gates; probes are tagged and
  reverted.** The toolkit includes `OGRE_TEX_TRACE`, `OGRE_RAM_KEY`, `OGRE_DMA_TRACE`,
  `OGRE_SCENE_LOG`, `OGRE_DL_TRACE`, `OGRE_CAPTURE_PRESENT` and the live console (AGENTS 6, 12).
- **The dump tools read both RDRAM image layouts and work in ROM order.** `OGRE_RAM_KEY_FULL`
  writes ROM order; `OGRE_DUMP_RDRAM` and the console's `dump`/`snap` write the host buffer
  (byte-reversed words). `rdram.load_image` detects the layout from `jr $ra` in the main segment
  and converts, so every tool reads the guest word at `a` as the bytes at `a & 0x1FFFFFFF`,
  big-endian (sessions 132, 145).
- **A whole-RDRAM dump holds the rendered frame in the game's framebuffers** (`0x800A8204` =
  `{0x80000400, 0x80025C00, 0x8004B400}`), and `tools/ogre_frame.py` renders it (session 133).
- **A slow screen is diagnosed with a host stack sample.** `OGRE_PROFILE` names the most recently
  entered function, and a profile address does not name a function when several units share that RAM
  (sessions 98, 112).
- **Drive the New Game opening from the title.** `OGRE_STEP` changes the sequence's own state, and
  an `OGRE_SCENE` entry lacks the pre-state a natural path builds (sessions 36, 53, 54).
- **An input replay is anchored on game state;** the VI retrace clock is wall-clock (session 70).
