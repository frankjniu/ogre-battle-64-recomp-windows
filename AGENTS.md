# AGENTS.md — rules for AI agents working on this port

This repo reverse-engineers and recompiles one N64 ROM for Windows, across many
sessions and many agents. The most common failure in it is an agent inferring
intent from code and then building a whole session on that inference. Read this
file first.

Companion docs:

- `PLAN.md` — where the port stands and the to-do list.
- `docs/README.md` — doc index.
- `docs/DECISIONS.md` — the decisions that still bind the port.
- `docs/guides/app-build.md` — Windows setup, build, run, every `OGRE_*` knob,
  and the diagnostics toolkit.
- `docs/guides/emulator-first.md` — read before interpreting a display list.

---

## Writing style — this applies to every word you produce

> **Write in simple, technical English. No mannerisms ("it's x, not y", "why x
> matters", etc).**

This covers docs, code comments, commit messages, and everything you say in
session to the developer.

- One claim per sentence. Name the mechanism, the address or the file, and the
  evidence.
- Do not use rhetorical questions, "not just x but y", "the real question is",
  or headings that promise significance. Say what the thing is.
- Do not tell the reader what they believe, want or should care about. State
  the fact.
- Drop emphasis words that carry no information: "crucially", "importantly",
  "the key insight", "what matters here".
- Write a plain statement instead of a contrast pair: "`func_80178568` is the
  enter; `func_80178954` is the update", not "`func_80178568` is the enter, not
  the update".

---

## 1. Ask the developer what a scene should display

The assembly gives mechanism. Only the developer knows intent; they have played
the game. Treat the developer as the oracle for what a scene shows and does.

> **If you find an unusual data structure, a table you cannot classify, or a
> branch you cannot name, stop and ask the developer what the current scene
> should display or do before building a theory on it.**

Ask early, in batches, and attach the evidence plus your candidate answers so the
developer only has to choose:

> "Scene `0x0D`'s enter takes two branches on `D_8018F1C0`: `0` loads records
> 10a/10b and installs a vtable at `&D_801E5AC0`; non-zero runs
> `func_80226FA8` and sets a selector. Which one is the intro movie, and should
> the first visit be step 1 or step 0?"

Questions that resolve intent: what should this screen show; is this a movie, a
form, a menu or a cutscene; which button or flow gets here; does the real game
reach this; what text or appearance should the player see; is X supposed to
happen automatically. `docs/scenes.md` records the answers so far.

## 2. Verify written claims at instruction level

Prose in these docs or in a commit message is a lead. Verify it before repeating
it.

- Disassemble: `mips-linux-gnu-objdump -d build/bankC.elf
  --start-address=… --stop-address=…` (also `build/ogrebattle64.elf` and the
  other `build/bank*.elf`). On Windows the MIPS binutils live in the WSL tree
  (`docs/guides/app-build.md` → "Windows setup"). Read the raw ROM bytes when
  endianness or mapping is in question.
- Never trust generated C alone for control flow. The recompiler expands jump
  tables, splits functions and emits fall-through calls. `BankCFuncs/` and
  `RecompiledFuncs/` are generated and gitignored, so a fact read there may come
  from a probe or an older patch.
- When you correct an earlier claim, say so, give the instruction addresses, and
  fix the doc that made it.

## 3. Verify the consumer before naming a struct, table or field

Do not infer a layout from the code that writes it. Find the code that reads it.
`D_800AF028` is an accessor table (scene id → function returning the scene
descriptor), and not a table of update functions; the descriptor words are
`+0x00` enter, `+0x04`/`+0x08` per-frame hooks, `+0x0C` leave and `+0x10`
bank-record mask. One word can have several consumers (rule 5).

## 4. Check for mis-binding before blaming game logic

Several walls were the recompiler binding a call to the wrong function, or to the
wrong half of a function. Check these first.

- **Size overrides** in `config/config.toml` and `config/banks/config-bank*.yaml`.
  An override that runs past the next function's start makes the recompiler
  redirect every `jal` into it.
- **Fall-through and merged tails.** `make midfunc` lists every prologue-less
  tail with the frame slots and registers it reads before writing, and the `jal`
  sites that reach it. A prologue-less `jal` target in one bank is a hint that
  the call belongs to another bank, and not proof of a shared tail; check which
  bank is resident first.
- **Cross-bank fixed-address calls.** Records overlap in RAM by design
  (`0x80197B90` holds records 0/1/2/15/17), so a fixed-address call can land in a
  different resident bank. `python3 tools/cross_bank.py report` lists the sites;
  `dispatch --only …` is wired into `make recomp`, and the notes under the
  `recomp` target say why each target is there. Bank code compiles into
  `Bank{A..Z,AA..AH}Funcs/` (34 units) and is registered at runtime in
  `app/src/bank_overlays.cpp`. `make bank-recomp` runs `cross_bank.py
  check-banks`; `make cross-bank-check` is the full audit.
- **A `jal` compiled as "call the containing body, then `return`"** abandons the
  caller's frame. The frame-pump thread (thread 4, `func_8008AFE0`) dies when its
  saved registers are read from the wrong slots: `D_800AEFA4` (frame counter)
  freezes while `D_800C4BCC` (VI retrace) keeps counting. To find a leaking call,
  log `ctx->r29` before and after each call in the chain.
- **Register-relative stores and loads are invisible to symbol greps.** Grep the
  symbol and the offset/call sites, and read the function. A `lui r,0x8023` with
  a negative immediate wraps (`0x80230000 + 0xFFFFA994 = 0x8022A994`).
- **When a call runs the wrong code, ask the port first.** `grep "UNKNOWN module"
  run.log` and `tools/rdram.py <dump> banks` answer in seconds which module is
  resident and which one the port has.
- **Use the toolkit instead of writing another throwaway script.** All of it is
  in `docs/guides/app-build.md` → "Diagnostics toolkit":
  - `tools/runlog.py <run.log>` — scene timeline, RSP tasks by type (including
    any non-gfx task the stub swallows), bank loads and problems. `--check` turns
    it into an assertion. Run the game with `OGRE_RSP_LOG=1` (and
    `OGRE_DL_TRACE=1` for the display-list cadence); a stock run prints neither.
  - `tools/guestmap.py <addr>` — rom↔vram, owning record/unit, function-entry
    check, and which other records share this RAM.
  - `tools/rdram.py <dump> image|diff|banks|word|…` — RDRAM image reads.
  - `tools/scenemap.py` — the 25 scene types, every writer of the pending-scene
    word, and the scripted step table, from the ROM.
  - `tools/arenamap.py` — every streamed arena bank and whether a unit links it.
- **For "what is in RAM at the moment X happens", use the live console**
  (`OGRE_LIVE_CONSOLE=1`) or a key-triggered dump (`OGRE_RAM_KEY`), not a bounded
  run's exit dump. `save`/`load` checkpoints rewind the machine and are valid
  only for the process and binary that wrote them.
- Chunk-DMA records may never be DMA'd at all (the `0xD0` gap), and record BSS
  must be zeroed on load (`load_function_bank` and `RAM_END` in
  `tools/gen_bank_funcs.py`).

## 5. Isolate one variable per experiment

An A/B run must change exactly one thing, or the result cannot be interpreted.
Poking `D_8018F1C0` at scene `0x02` to test the `0x0D` enter also changed the
`0x02` loader, which reads the same word. When you cannot isolate, say so and
name the confound. Intermittent faults need a batch: one boot in four froze in
the test that rejected `yield_self` for the idle loop.

## 6. Revert probes by regenerating, and prove the revert

- Instrument generated code (`RecompiledFuncs/*.c`, `Bank*Funcs/*.c`) or the app.
  Never a submodule. Tag every probe (`// probe146`) so it can be grepped.
- Revert by regenerating (`make recomp && make bank-recomp` in the WSL tree) and
  copying the output back, then rebuild.
- Prove it: `grep -rl probe146 RecompiledFuncs/ Bank*Funcs/ app/` must be empty.
- Two RDRAM image layouts exist. `OGRE_DUMP_RDRAM=<path>` and the console's
  `dump`/`snap` write the raw host buffer (`fwrite(rdram, …)`): words are
  little-endian at `a - 0x80000000` and logical bytes are XOR-3, as in live host
  RDRAM. `OGRE_RAM_KEY_FULL` writes ROM order (`logical_byte`, `rdram[off ^ 3]`):
  the guest word at `a` is the four file bytes at `a & 0x1FFFFFFF` read
  big-endian. The dump tools load either through `rdram.load_image`, which
  detects the layout and converts a raw image to ROM order.

## 7. Do not fabricate state to get past a crash

Do not invent a buffer, a flag, a return value or a "safe" branch to make a wall
go away. A path that only works with state the game never sets is a finding;
write it down and say what it implies. A repair is legitimate only when it
reproduces what the game's own code or data does.

## 8. Retail-behaviour claims need hardware evidence, and a port display list is not that evidence

A display list or decoded asset that the port produced is evidence about the
port until the port's faithfulness is established. Run the five checks first;
recipes are in `docs/guides/emulator-first.md`:

1. **Which GBI did this list get?** Hash the task's ucode with XXH3-64 and look it
   up in RT64's database. Never infer a ucode from `OGRE_DL_DECODE` or
   `OGRE_DL_ANALYZE`; they decode every list as F3DEX2.
2. **Did an RSP task go missing?** `tools/runlog.py <run.log>` flags non-gfx tasks
   the stub swallowed, from a run with `OGRE_RSP_LOG=1`. `app/src/rsp.cpp` runs
   the recompiled njpeg and audio microcodes and stubs every other task.
3. **Is the code the code we compiled?** `tools/rdram.py <dump> banks` and
   `make midfunc`.
4. **Are the bytes what hardware would have?** Decode an asset with an
   independent implementation (`tools/ogrelz.py`) and require the stream to end
   on its declared payload boundary.
5. **Is the recompiler faithful at this call?** Compare the generated C at the
   `jal`, the bank ELF and the raw ROM bytes. A `jal` delay slot runs exactly
   once, before the callee; the duplicate N64Recomp emits after `goto after_N`
   is dead code.

Then state both hypotheses and name the cheapest discriminating experiment: a
runtime watcher, a hardware watchpoint on `ultramodern::get_rdram_base() +
(guest_addr - 0x80000000)`, a live-console dump, or a reference emulator
(RetroArch with mupen64plus-next or parallel-n64; GLideN64 ships
`[OGREBATTLE64] graphics2D\enableTexCoordBounds=1` and `hack_Ogre64`). If nothing
in the run ever writes a slot that a path needs, the path is dead in the port and
on hardware for the same reasons.

## 9. Timing and harness caveats

- `OGRE_TAP_MS`, `OGRE_EXIT_AFTER_MS` and `OGRE_SPEED` are wall-clock.
  `OGRE_TRACE_HOOKS=1` slows the boot several-fold.
- `OGRE_SPEED=n` scales the emulated clock, so a fixed wall-clock tap point moves
  relative to the game.
- Scene forcing (`OGRE_SCENE=…`) enters a scene without the pre-state a natural
  path builds, so "forced X crashes" does not mean "X is broken".
- No repo test harness exists. Each check is a ROM-dependent game run of 15-170
  s. Disclose that; do not invent a framework.

## 10. Keep the record short and current

- `PLAN.md` is the to-do list. Delete an item when it is done, and add new ones
  with what is known and the next step.
- Update the guide that describes a changed behaviour (usually
  `docs/guides/app-build.md`).
- Add an entry to `docs/DECISIONS.md` only for a decision that binds future work.
- Do not write per-session handoffs or status logs. The git log is the history;
  a commit message states the goal, the result and the evidence.
- Never commit ROM dumps, extracted assets, or changes inside `tools/`
  third-party trees (those go into `patches/`; `tools/patchcheck.py` must pass).
- `git status --short` before you finish must show only intended changes.

## 11. Do not drive the game yourself; ask the developer

The developer plays the game and drives it to the state a bug needs. Ask for that
state in their terms ("the cutscene where the knight's head goes black") instead
of scripting a route. A scripted route costs more than it looks: taps land in the
wrong scene, and the run is discarded.

- A save is their instrument too: `OGRE_SAVE=<path>` loads a battery image.
- Scripted taps (`OGRE_TAP_*`) stay for exact, short routes such as the boot.
- Ask for the artefact in a form you can read yourself (a file on disk, a
  screenshot), not terminal output to copy.

## 12. Instrument the game with debugging shortcuts, and have the developer press a key

A bug that appears in a played state is investigated by instrumenting the run and
pressing a key at the moment of the bug.

- **A key arms the capture** and a timer closes it (`OGRE_TEX_TRACE_KEY`,
  `OGRE_TEX_TRACE_MS`), so one press captures the bad frame and nothing else.
- **A key writes a file** (`OGRE_RAM_KEY=<1..9>` with `_ADDR`, `_LEN`, `_OUT`,
  `_FULL`), read from disk afterwards.
- **Answer offline what the log can answer**: print a fingerprint per image or
  slot rather than a dump per frame.
- **Filter at the source**, and **bracket the bug** with a dump on each side of
  the change.
- Probe rules still hold (rule 6). A diagnostic that is part of the toolkit
  (`OGRE_TEX_TRACE`, `OGRE_RAM_KEY`) is not a probe and stays.

## 13. Start debug runs at the title screen

`OGRE_SCENE=title` targets the title scene `0x04`; pair it with `OGRE_SAVE=<path>`
to land with a battery loaded, and keep `OGRE_SCENE_LOG=1` for one `[scene]` line
per change. A forced scene enters without its natural pre-state (rule 9).

## 14. Launch every game run with no audio

Set `OGRE_NO_AUDIO=1` on every run, in scripts and in documented command lines.
It skips opening an output device (`app/src/sdl_platform.cpp`); the audio
microcode still runs.

## 15. Windows is the target

The developer targets Windows only (2026-10-30). Verify every change on the
Windows build. Do not spend time on the browser build (`make web`), macOS or
Linux unless the developer asks. Shared sources still must not include a
native-only header without an `#ifndef __EMSCRIPTEN__` guard, because that costs
nothing.

---

## Durable facts

Re-verify if in doubt; otherwise start from here.

- **Uninitialised guest state holds the game's own leftovers, not noise.** The one
  systemic difference from hardware is that the port zero-fills RDRAM at boot.
  When the port has `0` where retail clearly does not, check the address map
  (`recomp_mem_addr`) and the bank map before suspecting game logic.
- **Vocabulary and intent:** `docs/symbols.md` (address → name → evidence →
  confidence) and `docs/scenes.md` (what each screen should show, from the
  developer).
- **Scene dispatch.** `func_80075BC0` writes 25 accessor pointers into
  `D_800AF028[0..24]`; ids ≥ 0x1F map to index 0. The descriptor table is static
  ROM data (`streamedB`, ROM `0x40E80` + (vram − `0x8016AF80`)). `D_800C4C26` is
  the current/pending scene word (`0x8000|id`, plus `0xFFFC`/`0xFFFE`). 39 store
  sites in 30 functions write it; the scene-script VM `func_80170974` writes the
  step word `D_8018F1C0` and the next scene `D_8018F1C2`.
- **Scripted step table:** asset `0x19A8804` (ROM `0x1F3CA54`), a `u32` size
  `0x1A74` then 1693 `u32` per-step asset ids, indexed by `D_8018F1C0 & 0xFFF`.
  The 200+ dialogues are entries in this table, not scene types.
- **New Game** is `title (0x04) → 0x02 → 0x0D → 0x02 → 0x0D → …`. `0x02`
  (`D_8018FC50`, enter `func_80178920`) is a one-frame loader. `0x0D`
  (`D_8018FC3C`): enter `func_80178568`, update `func_80178954`, hook
  `func_80178B40`, leave `func_80178B7C`, mask `0x40007C14`; `D_8018F1C0 == 0` or
  bit 15 selects movie mode (records 10a/10b), non-zero selects the cutscene
  engine (records 14a/14b, `func_8022683C(-5, F1C0 & 0xFFF)`). Steps ≥ 2 stream
  `bankRec14c` over `bankRec14b` at `0x802395E0`; step 1073 (the Prologue card)
  streams a third bank of the record-14 arena (ROM `0x286BA0` → `0x8022ACB0`).
  The engine state is at `0x8022A970` (pc), `0x8022A978` (descriptor base),
  `0x8022A994` (engine struct). Drive the opening from the title
  (`OGRE_TAP_BUTTON="start,a,start,a,…"`); seeding `OGRE_STEP` skips step 1 and
  takes the movie-mode branch. The attract loop (`title ↔ 0x0B / 0x0C`) never
  enters `0x02` or `0x0D`.
- **Scene `0x07`** (name-entry form): descriptor `D_8018FDAC`, enter
  `func_8017B794`, streams ROM `0x712A0` (0x84B0) → `0x8019A7C0` (unit H).
  **Scene `0x05`** (map): descriptor `0x8018FD70`, enter `0x8017B60C`, mask `0x2`,
  module ROM `0x79750` (0xDAD0) → `0x8019A7C0` (unit M). A bank's size is its
  DMA's; compare the chunk's rom→ram delta, not the ROM range alone.
- **Do not compile a swappable RAM range into the unit whose code calls into
  it.** Put each bank of an arena in its own unit; N64Recomp then emits
  `LOOKUP_FUNC(addr)` and the runtime's DMA-driven bank map picks the resident
  module. Find unknown modules by logging every PI DMA destination and diffing
  against `bank_funcs.inc`'s record table.
- **njpeg backgrounds.** CPU Huffman decode (`func_8008B250`) → four
  `M_NJPEGTASK` (type 4) RSP tasks that decode to 16-bit YUV (`ucode=0x8009ED80`)
  → a second gfx ucode `0x800A5110`, `S2DEX 2.08`, whose per-macroblock `0xDC`
  is `G_OBJ_MOVEMEM` and `0xDA` is `G_OBJ_RECTANGLE_R` (the draw) → a CPU
  framebuffer readback → a `'B5'`-headed image at `0x80243E10`. A YUV
  `LOADTILE` is the RDP's two-plane format (luma upper TMEM, U/V lower, stride
  `line << 3`); the source word is `[U, Y(even), V, Y(odd)]`. The colour matrix is
  the game's own `G_SETCONVERT` (`k0=175 k1=469 k2=423 k3=222`), each 9-bit field
  sign-extended and scaled `2*K+1`; rows `R = Y + K0*V'`, `G = Y + K1*U' + K2*V'`,
  `B = Y + K3*U'`. The readback source is the game's `state[0x64]`, not RT64's
  stale scratch word `0x807FFC08`. In scene `0x02` the four stage-3 copies in
  `func_ovlE_8019976C` fill `0x801AAE90` / `0x801D06B0` / `0x801E50D0` /
  `0x801FB8F0`, the four chunks of the 2x2 background. See
  `docs/guides/njpeg-backgrounds.md`.
- **The map screen's sprites are faithful** (LZ decode exact, the map ucode
  `0x8009F540` is `F3DEX2.fifo 2.08`). The knight is composited at run time into
  a `malloc(0x18000)` RGBA32 sheet at `state[+0x34]` (24 frames, `0x1000` each);
  `state` is `*(0x80197B18)`. Do not "fix" entry indices or texture offsets in
  generated C.
- **RSPRecomp's `text_address` is a label base**: the IMEM DMA address masked
  `0x1FFF` (`0x1080` for a ucode loaded at IMEM `0x080`), not the text's RDRAM
  address. A wrong value rotates every `j` target.
- **The VI scans out from `framebuffer + origin`** (`0x280` for standard NTSC
  modes, one 640-byte row), so resolve a scanout buffer by containment. The
  game's framebuffers are `0x000400`, `0x025C00` and `0x04B400` (320x240
  RGBA5551, pitch 640); the world map adds three 640-wide targets
  (`0x1BA780`, `0x250800`, `0x2E6840`). A 16-bit framebuffer written back to RDRAM
  puts value `V` at `off ^ 2` (low byte) and `off ^ 2 + 1` (high byte).
- **Threads.** N64 threads run as native threads. Thread 1 (`func_8007F8E4`)
  boots, starts thread 3 (`func_80071EB0`, the game flow), drops to priority 0 and
  loops forever on an idle callback at `0x800E7A18` that nothing writes; a
  `[[patches.hook]]` sleeps it about 1 ms per pass (`ogre_idle_wait`). Do not use
  `yield_self` there: it puts the idle thread into the N64 scheduler and froze a
  boot in testing. Poll loops get `yield_self` from the recompiler's heuristic;
  misclassified work loops are listed in `yield_work_loop_branches` in
  `config/banks/config-bankN.toml`.
- **Windows host behaviour.** The process is per-monitor DPI aware
  (`SDL_HINT_WINDOWS_DPI_SCALING`): window sizes are points, the swap chain is
  pixels. A redirected stdout/stderr keeps its file; a stream with no handle is
  reopened on `NUL` on fd 1/2 so `crash_log` can capture it
  (`app/src/main.cpp`). The runtime names its threads (`SetThreadDescription`),
  so per-thread CPU can be attributed by name.
