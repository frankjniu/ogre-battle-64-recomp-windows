# Decisions

The decisions that still bind the Windows port. Each entry is the current rule.
When a decision changes, edit its entry. The evidence and the superseded entries
are in this file's git history (`git log -p docs/DECISIONS.md`).

## Scope

- **The port targets Windows only.** The browser, macOS and Linux builds are not
  built or verified unless the developer asks. A source shared with the browser
  build still guards native-only includes with `#ifndef __EMSCRIPTEN__`.
- **The app is GPL-3.0.** N64ModernRuntime is GPL-3.0 and statically linked;
  N64Recomp and RT64 are MIT. `make dist` copies `LICENSE` into the package as
  `LICENSE.txt`, and `tools/smoke-dist.sh` fails a package without it.

## Build and code generation

- **`make regenerate` runs `resplit` → link the ELF → `bank-recomp` → `recomp` →
  `rsp-recomp`, in that order.** `make recomp` dispatches cross-bank calls
  against the records `make bank-recomp` writes to `app/src/bank_funcs.inc`;
  without them the build succeeds with 15 calls bound to the wrong bank, so
  `recomp-prep` fails when the file is missing.
- **Generated code is never committed.** `RecompiledFuncs/`, the 34
  `Bank*Funcs/`, `RspFuncs/` and `app/src/bank_funcs.inc` are a translation of the
  ROM. On Windows, `make regenerate` runs in a WSL clone and the outputs are
  copied into the checkout.
- **Configs live under `config/` and upstream patches under `patches/`.** splat,
  N64Recomp and RSPRecomp resolve a config's paths against its own directory.
- **Clear a unit's generated outputs before regenerating it.** splat and
  N64Recomp never delete a previous run's files. A `data` subsegment needs an
  explicit `bin` gap after it.
- **Every linked ELF must be byte-identical to the ROM, and a misalignment is
  fixed in the config.** `make elf-rom-check` (`tools/elfcheck.py`) runs inside
  `make bank-recomp`.
- **`make resplit` always leaves `asm/1CE040.s` and `asm/40E80.s` dirty.** Do
  not commit or revert them unless the label patch changes.
- **`make bank-recomp` re-applies `tools/njpeg_readback.py` (unit E's njpeg
  readback) and `tools/hd_backgrounds.py` (the source and pass hooks in all four
  njpeg copies: units E and C and, from `make recomp`, the title module in
  `RecompiledFuncs`),** so the edits survive regeneration.

## Recompiler (N64Recomp)

- **libultra is bridged by name.** The game's libultra functions are named in
  `config/symbols/symbol_addrs*.txt`, and `reimplemented_funcs` binds them to the
  runtime's `osXxx_recomp`.
- **`func_80089A10` (a yielding wait) and `func_8008BC40` (a synchronous ROM
  read through `recomp::do_rom_read`) are reimplemented in the runtime.** A plain
  `memcpy` of ROM bytes gives byte-swapped pointers.
- **A `function_sizes` override ends on the function's own last instruction (its
  `jr ra` delay slot) and never runs past the next function.** A `jal` into a
  size-extended body compiles as a call to the containing body plus an early
  `return`, which leaks the caller's frame (AGENTS.md rule 4).
- **The poll-loop heuristic stays general; a misclassified work loop is fixed per
  site in `yield_work_loop_branches`** (`config/banks/config-bankN.toml`). The
  injected `yield_self` waits about one VI retrace per iteration. Three broad
  heuristic changes deadlocked the boot.
- **Host behaviour inside a game function goes in a `[[patches.hook]]`.** The
  idle thread's hook at `0x8007F998` calls `ogre_idle_wait`
  (`app/src/overlays.cpp`), which sleeps about 1 ms per pass. `yield_self` there
  froze one boot in four.
- **The trace hooks (`recomp_trace_entry`/`recomp_trace_return`) stay.**
  Removing all of them did not change a 30 s boot's CPU time.

## Banks and cross-bank calls

- **Streamed records compile as bank units partitioned by RAM, and the game's PI
  DMA drives the swap.** The 34 units come from
  `config/banks/config-bank<U>.{yaml,toml}` and register in
  `app/src/bank_overlays.cpp`. `notify_rom_read` loads the bank whose ROM range
  and rom→ram delta match the DMA. A bank's extent is its own DMA size.
- **A swappable RAM range is never compiled into a unit whose code calls into
  it.** Each bank of a shared range gets its own unit, so N64Recomp emits
  `LOOKUP_FUNC`. `make bank-recomp` runs `tools/cross_bank.py check-banks`, which
  fails on a hazard outside `tools/cross_bank_known_hazards.txt`.
- **Every `jal` from the main ELF into RAM a streamed module occupies is
  dispatched at run time.** `make recomp` runs `tools/cross_bank.py dispatch
  --only <list>`, which also repairs the tail-call emission. A new wrong-bank call
  is fixed by adding its target to that list.
- **Every module the game can stream needs a unit and a registered record.** An
  unknown record never evicts the previous bank, so `LOOKUP_FUNC` silently runs
  the old bank's body. `tools/arenamap.py` finds the loader-pattern banks;
  table-driven loads are found by diffing an `OGRE_DMA_TRACE=1
  OGRE_DMA_TRACE_FULL=1` census against the records.
- **A bank load erases only the function-map entries it covers and zeroes the
  record's BSS from the segment table.**
- **`get_function` returns a logging stub for an unknown streamed address.** The
  log lines `[overlays] streamed function stub called @` and `[bank] UNKNOWN
  module` name a missing module; `tools/stubmap.py` answers it statically.
- **A streamed lookup runs the body whose ROM bytes are resident.**
  `lookup_check` (`app/src/bank_overlays.cpp`) compares the resident words with
  the mapped body's ROM words up to its first `jr ra`; on a mismatch it calls the
  body that matches and logs `[lookup]` once per address. This executes the code
  the CPU would execute, so it is not fabricated state. It covers dispatched
  calls and `jalr`, not direct calls.
- **The main unit's direct calls into swappable RAM are bound correctly** (as of
  2026-10-31). A new one is checked with `stubmap.py target` and dispatched with
  `cross_bank.py dispatch --only` if another module can be resident.

## Runtime (N64ModernRuntime)

- **Runtime changes live in `patches/n64modernruntime-ob64.patch`.** Regenerate
  it with the nested N64Recomp excluded:
  `git -C tools/N64ModernRuntime diff HEAD -- . ':(exclude)N64Recomp'`.
- **The scheduler uses a strict handoff token (`running`) and a separate idle
  poke (`poke`); a drainer thread at priority 5 delivers external messages.**
  Higher numbers run first, so the drainer sits below the threads it wakes.
- **A host bridge that converts a guest pointer sign-extends it first.** The
  `MEM_*` macros subtract `0xFFFFFFFF80000000`.
- **`recomp_mem_addr` maps the KUSEG low window (`a < 0x80000000`) to
  `a & 0x003FFFFF`.** No current path is known to need it; A/B it before relying
  on it.
- **`osViSetMode` translates its argument only for a KSEG0 address, and
  `start_game` runs from a thread delayed 500 ms.** Without the delay the VI
  thread crashes on a null mode.
- **`osAiGetStatus` reports busy while more than 52 ms of audio is queued.** The
  game's audio thread is paced by `AI_STATUS_BUSY`.
- **GAME SPEED scales the CPU counter and the VI schedule through
  `ultramodern::set_speed_multiplier()`.** The clock is piecewise linear, so
  `osGetTime` and pending timers stay continuous across a change.
- **The audio task's reply on `0x800C49E8` comes only from the game's
  scheduler** (`func_80089200`, `0x80089324`). Every audio task reuses the
  command list at `0x80136110`, so an extra reply lets the CPU run a task ahead of
  the RSP.
- **The periodic `[snap]` queue snapshot is opt-in (`OGRE_SNAP`).** Each one
  costs about 1.6 s on Windows. A stalled frame counter runs it once and writes
  `error.log` (`app/src/hang_watch.cpp`, `OGRE_HANG_MS`).
- **Closing the window is a hard exit.** The runtime never joins the game
  threads; `main.cpp` ends with `crash_log::flush()` and `_Exit`.
- **Non-gfx RSP microcode runs recompiled (`RspFuncs/`), dispatched by ucode
  address in `app/src/rsp.cpp`.** `OGRE_NJPEG=0` and `OGRE_AUDIO_UCODE=0` force
  the stubs. RSPRecomp's `text_address` is the IMEM address masked `0x1FFF`
  (`0x1080` for both).

## Renderer (RT64)

- **RT64 interprets the gfx display lists with its auto-detected GBI.** The game's
  lists are F3DEX2 2.08 and the njpeg lists are S2DEX2 2.08. RT64 changes live in
  `patches/rt64-ob64.patch` and `patches/rt64-plume-ob64.patch`.
- **RT64 implements S2DEX2's `G_OBJ_MOVEMEM` (`0xDC`) and `G_OBJ_RECTANGLE_R`
  (`0xDA`), loads a YUV16 tile as the RDP's two-plane format, and converts YUV
  with the game's `G_SETCONVERT` matrix** (AGENTS.md durable facts).
- **The njpeg readback copies from the game's own framebuffer choice
  `state[0x64]`.** RT64's scratch word is stale at the first pass and is only a
  fallback.
- **A framebuffer tile copy is served only when the source framebuffer was
  written in the current workload and the tile lies inside its
  `lastWriteRect`; otherwise the load reads RDRAM.** Stale framebuffer records
  drew sprite parts as flat colour.
- **The CRT filters run in RT64's VI pass from a box-filtered copy of the frame,
  and only permissively licensed shaders are used** (crt-hyllian-fast, MIT;
  crt-lottes-fast and crt-lottes, public domain). A GPL shader would put the
  executable under the GPL. The MIT notice is in `packaging/README-dist.txt`.
- **RT64's "Fix LR with Scissor" is off.** It widened the Organize Screen's font
  rectangles by a column; `OGRE_RECT_LR_FIX=1` restores it.
- **The title fog repair (`OGRE_FOG`, on by default) gives the fog rectangles the
  layer image from the game's table at `D_801B80E0`.** The game leaves a
  zero-texel tile behind, which RT64 drew as a white band.
- **RT64's present and workload queue producers wait on a condition variable.**
  Polling held 96% of a core at GAME SPEED 8.
- **`OGRE_GRAPHICS_API=auto|d3d12|vulkan` pins RT64's backend at renderer
  creation.**

## App, settings and UI

- **Quick save is a slot of the battery save, and quick load restarts the
  program.** F5 copies `<game id>.bin` to `<game id>-quick.bin`; F9 (twice)
  writes the slot back and relaunches with `OGRE_RELAUNCH=1`. A RAM snapshot is
  unsafe because the game's call stacks are native (developer, 2026-10-31).
- **Cheats are built-in toggles at addresses checked against this ROM.** There is
  no custom-code file. A data cheat is held once per frame by `cheats_tick()`. A
  code cheat is a `[[patches.hook]]` that reproduces the instruction's result
  while the row is on, because RDRAM writes to code do nothing to compiled C. A
  write into a swappable bank's RAM is gated on that bank being resident. QUICK
  SOLDIER PROMOTION fills the leader's counter to 100 so one battle always
  promotes (developer, 2026-11-01).
- **UI text is drawn through `ui::TextCache`, and the start screen's renderer has
  vsync.** A cached text texture lives at least to the end of the frame that drew
  it.
- **Player settings live in `<config>/settings.cfg`; an `OGRE_*` variable
  overrides a key for one run without writing it back.** The old `resolution`
  key is ignored; RESOLUTION is `render_resolution`, default `auto`.
- **2D rectangles sample at the console's pixel positions (RT64
  `upscale2D = Original`).** RT64's default upscaled the game's slightly
  stretched dialogue box and read a texel past its window at 4x and above.
  `OGRE_UPSCALE_2D=scaled` restores it.
- **MSAA stays behind `OGRE_DISPLAY_EXPERIMENTS=1` (or a set `OGRE_MSAA`).** It
  produced artifacts and is unverified.
- **The window opens 1080 points tall and the process is per-monitor DPI
  aware.** Window sizes are points; the drawable is pixels.
- **A fullscreen window's geometry belongs to RT64.** `widescreen_fit_window`
  does nothing in fullscreen, and leaving it calls `widescreen_request_refit()`.
- **WIDESCREEN `on` opens a 16:9 window and expands the mission (`0x03`), the
  dialogue scene (`0x0D`), the world map (`0x05`), the Organize Screen (`0x06`)
  and scenes a backdrop pack covers.** The window shape never changes with the
  scene; other scenes are pillarboxed. `app/src/widescreen.cpp` owns RT64's
  `ar_option`.
- **A 2D screen is widened by patching its own draw with RT64 extended GBI, not
  by stretching** (`docs/guides/widescreen.md`). The developer chose (2026-11-01):
  on the map, more map with the frame at the edges; on the Organize Screen, the
  background across the full width, the counters and WAR FUNDS at the left edge,
  the name plate at the right edge, the grid centred.
- **GAME SPEED offers 1, 2, 4 and 8.** Speed 6 stays out because the cursors are
  too fast to aim.
- **The launcher and the `ESC` overlay share one tabbed panel
  (`app/src/ui.cpp`).** The overlay is a separate borderless window because
  RT64 owns the swap chain. The game keeps running while it is open.
- **The controller map is player data in `<config>/controls.cfg`, and `ESC` is
  never bindable.** Every gamepad and the keyboard feed N64 controller slot 0;
  the game reads only `OSContPad[0]`.
- **Mouse support is the MOUSE setting, off by default.** The edge scroll injects
  the game's own D-pad bits.
- **A mute is a gain of 0 in the audio callback, and the buffer is still
  queued,** because `audio_dma_busy()` paces the game from the queue depth.
- **The live debug console is off unless the run sets `OGRE_LIVE_CONSOLE=1` or a
  trigger's own variable,** because its commands write guest RAM.
- **A stock run logs boot lines and errors only.** `[rsp]` lines need
  `OGRE_RSP_LOG=1` and per-display-list lines need `OGRE_DL_TRACE=1`.
- **The ROM is resolved from the argument, `OGRE_ROM`, a ROM beside the
  executable, the stored copy, `assets/`, then the start screen.** The config
  directory is the executable's folder.

## Windows platform

- **The `.exe` is a GUI-subsystem binary with a `WinMain` forwarder.** A terminal
  launch attaches to the parent console; `OGRE_CONSOLE=1` allocates one.
- **A standard stream keeps a redirect it inherited, and a stream with no
  descriptor is reopened on `NUL`,** so `> run.log` and an Explorer launch's
  `error.log` both capture output.
- **`crash_log` mirrors fds 1 and 2 into rings and writes `<config>/error.log` on
  a fatal event.** The Windows handler uses no stdio.
- **The build uses clang-cl under Ninja with the static CRT.** `cl.exe` rejects
  the code's `-W` flags. `dxcompiler.dll`, `dxil.dll` and their VC runtime DLLs
  ship beside the `.exe`.
- **Patch files are LF everywhere (`.gitattributes`: `*.patch text eol=lf`).**
- **Windows Defender quarantines the binary after a scripted NULL-handle launch or
  a fault-injection stub.** Use an Explorer launch for that state.

## Saves

- **The game saves to 32 KiB of battery-backed SRAM**, written to
  `<config>/saves/<game id>.bin`.
- **A battery slot's checksums are seeded with the slot's device offset**, so a
  slot's data is valid only in its own slot. `tools/sramsave.py` imports
  emulator saves and reseeds (`docs/items.md` → save layout).
- **The Controller Pak is the copy/backup device**, bridged at
  `__osContRamRead`, `__osContRamWrite` and `__osPfsGetStatus` over a flat
  32 KiB image. Its copy and erase flow is out of scope (developer).
- **A checkpoint (console `save`/`load`) is valid only in the process that wrote
  it.** Hand a battery save between sessions.

## Mods

- **The mod system is on (`MOD_GAME_ID = "ogrebattle64"`, mods in
  `<config>/mods/`), and the start screen shows whenever a mod is installed.**
- **A code mod hooks only functions in the base sections (`.entry`, `.main`,
  `.streamedA/B/C`).** A mod that needs bank behaviour hooks the frame function
  `func_80072944` and works on RAM (`docs/guides/app-build.md` → "What a hook can
  reach").
- **Example mods ship disabled** (`enabled_by_default = false`, a field
  `n64recomp-ob64.patch` adds to RecompModTool).
- **A backdrop pack (`mods/backgrounds/`) replaces a whole njpeg backdrop at the
  readback, and HD detail comes from an RT64 texture pack keyed by strip hash,
  generated from the same PNG** (`docs/guides/hd-backgrounds.md`).

## Releases and packaging

- **The fork releases from the Windows machine** (`make dist-zip`, the smoke
  test, `gh release create --repo frankjniu/ogre-battle-64-recomp-windows`). The
  Release workflow and `make release` need a private data repository or a
  self-hosted runner; the fork has neither, and its workflow is disabled.
- **Each tracked patch must reproduce its vendored tree** (`make patch-check`,
  `tools/patchcheck.py`).
- **`tools/smoke-dist.sh` gates every package**: PE imports, a launch, a crash
  report with the `[boot]` line, and on Windows an Explorer launch with no
  standard handles.

## Diagnostics

- **Toolkit diagnostics stay in the build behind environment gates; probes are
  tagged and reverted** (AGENTS.md rules 6 and 12).
- **The dump tools read both RDRAM image layouts and work in ROM order**
  (`rdram.load_image`; AGENTS.md rule 6).
- **A whole-RDRAM dump holds the rendered frame in the game's framebuffers**, and
  `tools/ogre_frame.py` renders it.
- **A slow screen is diagnosed with a host stack sample.** A profile address does
  not name a function when several units share that RAM.
