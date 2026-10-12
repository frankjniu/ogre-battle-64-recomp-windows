# Handoff — 2026-10-11, session 132: the sprite mechanism, the dump byte order, and the one-part black-tile path

Developer request: *"read the latest handoff docs, and continue the investigation
around the black sprites that happen when some body ports animate. try to
understand the sprite mechanism really well (and document it), because I will
not be around to drive sessions. you have a special permission to drive the game
if needed."*

**Result.** The sprite mechanism is documented end to end in a new guide,
`docs/guides/sprite-mechanisms.md`, and the resolver's control flow is decoded at
instruction level (§4c). That decoding yields the first mechanism that produces
the developer's symptom exactly — **one part black while the rest of the
character draws** — with no bug required: the code stream's `0x8140` command
switches the working sheet, and a part whose code is not in the loaded sheet
resolves to `-1`, so its `0x70`-byte buffer is zeroed and the draw paints palette
index 0.

Two earlier-project errors are corrected (the dumped-RDRAM byte order, and the
record layout of the part cache) and four claims are withdrawn, two of them this
session's own. The black part is **not fixed**, and the reason is single and
concrete: **no capture on disk contains the fault** — every trace taken has
`0 'BLACK DRAW'` lines. §4c names the exact test to run on the next capture.

## 1. The dump byte order was wrong in both readers

`OGRE_DUMP_RDRAM` / `OGRE_RAM_KEY_FULL` images store bytes in **ROM order**: the
four file bytes at a guest address, read big-endian, are the guest word, and the
file byte at `a & 0x1FFFFFFF` is the logical byte at guest `a`.

The check is the part cache's own layout, which `func_8017909C`
(`asm/40E80.s:17123`) computes. Read in ROM order, the developer's capture
gives:

```
D_8018FC88  block base   = 0x80269640
D_80190F70  presence     = 0x80271640   base + 0x8000, 0x200 bytes
D_8018FC90  sheet A      = 0x80271840   base + 0x8200
D_8018FC94  sheet B      = 0x802718E8   base + 0x82A8
D_80190F74  code table   = 0x80271990   base + 0x8350, 0x1000 u16
```

Every offset is the one the loader computes, and the block size
`0x100*0x80 + 0x200 + align8(A) + 0x200 + align8(B) + 0x2000 + 0x200` = `0x14200`
is the `func_80071288` argument it builds. A byte-reversed read gives
`0x40962680`, `0x40162780`, `0x40182780`, `0x90192780`.

Fixed: `tools/rdram.py` (`word`/`half`/`byte`, and the `Dump.logical` transpose
used by `banks`) and `tools/ogre_ram.py` (`Image.word`/`half`/`byte`/`region`,
`find-ptr`). Session 131's "12x16 cell with a palette at `+0x60`" and its
`first non-zero at +72` came from the byte-reversed read and are withdrawn.

## 2. The part cache, verified at instruction level

One `0x80`-byte record holds one part: links at `+0x00`/`+0x04` (measured — 255
pointer words at each offset of the stride, and none at `+0x70`/`+0x74`), the
4bpp payload at `+0x08` (16 rows of 8 bytes, 12 texels wide), the `u16` code at
`+0x78`, the resolved flag at `+0x7A`.

`func_8017A740` resolves a code through the `u16` table at `D_80190F74` and
returns `index + 0x200` or `-1`. `func_8017A908` tests the presence bitmap at
`D_80190F70` and calls `func_8017AE40` (load) or `func_8017AD5C` (resident).
Both return **`record + 8`** for the image (`0x8017B0B0` `addiu v0,v0,8`;
`0x8017ADBC` `addiu v0,a1,8`), the loader DMAs the sheet row to `record + 8`
(`0x8017AF5C`, `a1 = s0 & ~1`), and `func_8017AC28` writes the expanded 4bpp
payload there (`0x8017AF74`, `a1 = D_80190F6C + 8`).

The sheet is not fixed: `func_8017A740` reselects one of three ROM assets
(`0x216DF1A`, `0x218DB76`, `0x218C17E`) by `D_8018FC8C` and `D_8018FC98`, and
`func_80179374` resets the selector on every flush — a scene that does not
re-issue its sheet command reads the wrong sheet, and every code the sheet lacks
resolves to `-1`.

A `-1` is a black tile **by the game's own design**: `func_801A8124` zeroes a
`0x70`-byte scratch when the code does not resolve (`0x801A81FC`, `0x801A8200`),
and scene `0x14`'s part preload zeroes a whole `0x70`-byte entry the same way
(`func_801AC944`, `0x801ACAE8`). Every byte of an all-zero 4bpp image is palette
index 0.

The guide also documents the scene/UI draw object (`func_801AC944` ->
`func_801ACE00` -> `func_801AB998`, worked through for scene `0x14`),
`sprite_rect_emit` and the map's sprite table, and a five-step check-a-draw
recipe.

## 3. Two claims made and withdrawn inside this session

**The draws bind `record` instead of `record + 8`.** The measurement tested for
a `u16` code at `timg - 8 + 0x78` and found 22 of 25. But the record's links are
at `+0x00`/`+0x04`, so `timg - 8` is the record *upstream* of the draw's record
and its `+0x78` belongs to that neighbour. Nothing observed shows a mis-rendered
text glyph — the dumped cells already decode as letters — so the text draws'
addresses are taken as correct. Withdrawn in `docs/notes-black-sprite.md` §3b.

**"The captured bytes are a 12x16 cell with a palette at `+0x60`."** That was
session 131's, and it came from the byte-reversed read. Withdrawn.

The lesson to keep: a `u16` in `1..0x1FFF` at an `0x80`-strided address in this
cache is not evidence, because the field is 2 bytes wide and the stride is
`0x80`. Only the link fields identify a record, and they are at `+0x00`/`+0x04`.

## 4. What the developer established about the fault

The black part is a **flat 2D character in a cutscene/dialogue close-up**
(developer, session 132). The head renders correctly at the start of the shot,
and **the fault appears when the character moves its head down and the game
swaps the head sprite**. So it is per-part and per-pose, not per-shot, and the
*replacement* sprite is what goes black.

Two shapes fit that, and a capture separates them:

- the replacement head is a different code (or a different frame of one code)
  that does not resolve on this visit, so a consumer zeroes the buffer;
- the replacement is composed into a second buffer on the frame of the pose
  change, and that compose does not run, or runs into a buffer the draw does not
  read.

**The instrument is a before/after pair of full RDRAM images.**
`OGRE_RAM_KEY=1 OGRE_RAM_KEY_FULL=1` writes a new whole-image dump on every
press, so one run gives `ram-1-full.bin` (before the pose change) and
`ram-2-full.bin` (at the black frame). `tools/rdram.py diff A B` groups the
differing bytes by module, and the buffer the replacement uses is among them. A
pair with **no** difference in the sprite buffers means the draw reads the same
bytes in both poses, which points at the palette or the address rather than the
fill.

## 4b. What the two pose dumps do and do not show

The developer's whole-image pair (`black-sprite/ram-1-full.bin`,
`-ram-2-full.bin`, one press each side of the head-pose change) differ by
**263124 bytes (3.14%)**. Session 131's "9 changed bytes" reading is withdrawn.

Two explanations of that difference were proposed in this session and **both are
falsified by measuring the cache's code fields**:

| pose | records in use | distinct codes | free (`0x1FFF`) |
|---|---|---|---|
| A | 32 | 32 | 224 |
| B | 35 | 35 | 221 |

- "The free list and the payloads disagree, so the cache is corrupt." No:
  `0x1FFF` is what `func_8017909C` writes into every record at initialisation
  (`0x80179184`) and it `bzero`s only the presence bitmap, so unused records keep
  stale bytes. That is the normal free-list state.
- "The cache is churning under pressure and recycling a record a draw still
  points at." No: only **3 records change code** between the poses (slots 32, 33,
  34), and the cache is **35 of 256 records full**. There is no eviction pressure.

So the two dumps tell us the scene asked for three more parts in the second
pose, and nothing more. They do **not** contain the black part: no draw in the
11:21 trace is black (`0 'BLACK DRAW'` lines), and the trace window the developer
armed does not hold the fault frame.

## 4c. The mechanism that can black exactly one part, at instruction level

Round 3 decoded the resolver's control flow (`func_8017A740`,
`0x8017A750`..`0x8017A828`), and it yields the first mechanism in this
investigation that produces the developer's symptom — **one part black while the
rest of the character draws** — without needing any buffer to be zeroed by a
bug:

```
lbu  a0,0(cursor)          ; leading byte
jal  func_8017B570         ; 1 iff 0x81 <= a0 <= 0xFC
beqz v0, .sheettail        ; not a part command
```

- the predicate is exact: `0x81 <= code <= 0xFC` (`0x8017B574` rejects `< 0x81`,
  `0x8017B58C` rejects `>= 0xFD`);
- a code that fails it goes to the **sheet tail** at `0x8017A83C`, which picks
  the working sheet from `0x216DF1A` / `0x218DB76` / `0x218C17E` by
  `D_8018FC8C` and `D_8018FC98`, stores it in `D_80190F60`, clears the flag
  (`0x8017A858`) and returns **`-1`**;
- **`0x8140` is the stream's sheet-switch command** (`beq $v1,0x8140` at
  `0x8017A7BC`), and it takes the same tail, so it also returns `-1`;
- every other in-range code is binary-searched in the `u16` table at
  `D_80190F74`, returning `index + 0x200` or `-1`.

So when the working sheet is not the one that carries a part's code,
`func_8017A740` returns `-1`, the consumer zeroes a `0x70`-byte buffer
(`func_801A8124` at `0x801A81FC`/`0x801A8200`), and the draw paints palette index
0. The rest of the character, whose codes are in the loaded sheet, still draws —
which is exactly "the head was replaced with a black texture" while the body is
fine. The sheet choice is script-driven, and the two routes into the cutscene
leave the script in different places, which is the shape of the path dependence.

**Test, once a capture holds the fault:**

1. from the faulty `timg`, read the record's code at `+0x78` and look for it in
   `D_80190F74`: absent means the `-1` path;
2. read `D_80190F60` (the working sheet, a ROM address) plus `D_8018FC8C` and
   `D_8018FC98` in both pose dumps and check whether the working sheet changed
   across the pose change.

Neither has been run, because every trace taken so far has `0 'BLACK DRAW'`
lines — **the fault is in no capture on disk.**

## 4d. What is needed, stated plainly

The character-art fault has **never been captured on screen**. Every root-cause
hypothesis in this session was tested against the existing dumps and falsified,
so the honest status is: the mechanism is documented, three earlier sessions'
claims are withdrawn, and the fault itself is unlocated.

The step that would locate it needs a driven run and cannot be done from the
dumps on disk:

1. `debug/black-sprite-capture.sh`, pressing `1` while the first (correct) head
   is up and `1` again at the black frame, then `grep -c 'BLACK DRAW' run.log`
   to confirm the capture holds the fault;
2. with the faulty `timg` from that `BLACK DRAW` line,
   `tools/watch.sh <timg> --after <symbol>` names the routine that last wrote the
   buffer, and whether that ran before or after the frame's display list was
   built.

`OGRE_CAPTURE_PRESENT` yields black PPMs on this machine, so the screenshot has
to come from the desktop.

## 5. What was run for verification

| run | evidence |
|---|---|
| `python3 tools/rdram.py <dev dump> word 0x8018FC88 4` | `0x80269640`, `0x00000000`, `0x80271840`, `0x802718E8` — the cache layout, which only ROM order produces |
| `python3 tools/rdram.py <dev dump> banks` | every resident module reports exact whole-window matches (`4096/4096 bytes match`), which needs the logical view in ROM order |
| pointer-offset histogram over the slot table | 255 in-pool pointer words at stride offset 0 and 255 at offset 4; none at `+0x70`/`+0x74` — the record layout |
| `python3 tools/ogre_ram.py <dev dump> cell 0x8026A5C8 --slot 0x80` | `nonZero 22/128, mean 8.7, first non-zero at 0x48`, payload rows `33 13 13 30` / `23 13 12 30` / `01 11 11 10` |
| `OGRE_NO_AUDIO=1 OGRE_SCENE=title OGRE_EXIT_AFTER_MS=8000 ./build-dist/ogrebattle64` | exit 0, boots to scene `0x04` |
| a 40 s run on `saves/scene_14.bin` with a scheduled full dump | exit 0; the route was still in the publisher stills and `D_8018FC88 == 0` (the "cache not loaded" signal) |
| a developer-driven 90 s run on `saves/scene_14.bin` with `OGRE_TEX_TRACE` + `OGRE_CAPTURE_PRESENT` + `OGRE_RAM_KEY_FULL` | title → `0x12` → `0x05` → New Game → `0x0D`; the trace holds 6549 colour rows and **0 `BLACK DRAW`** lines, and the plane the frame binds most (`0x802FD700`) has byte mean 83.9/255. **No draw was black at the press**, so that capture does not contain the fault. `OGRE_CAPTURE_PRESENT` produced 868 PPMs that are black or nearly black — the presented-frame readback is not giving pixels on this machine, so a screenshot cannot come from it |
| `python3 tools/rdram.py diff <pose A> <pose B>` | `263124 byte(s) changed (3.14%) in 2887 run(s)`, most in heap/asset space — the two pose dumps are not near-identical |
| code-field census over the 256-record table in both poses | 32 used / 32 distinct / 224 free in pose A; 35 / 35 / 221 in pose B; only 3 records change code — **no eviction pressure**, which falsifies both the "cache corrupt" and the "cache churning" readings of the 263 KB difference |
| `grep -c 'BLACK DRAW'` on the developer's trace | 0 — the trace window does not contain the fault frame |
| instruction read of `func_8017A740`, `0x8017A750`..`0x8017A828` | `jal func_8017B570` then `beqz v0,.sheettail`; `0x8140` takes the same tail; in-range codes binary-search `D_80190F74` — the one-part black-tile path of §4c |
| instruction read of `func_8017B570`, `0x8017B570`..`0x8017B5A4` | returns 1 exactly when `0x81 <= code <= 0xFC` (`sltiu` at `0x8017B574` and `0x8017B58C`) |
| `word 0x80190F60` in both pose dumps | `0x027203D2` = `func_8009DD28(0x218C17E)`, the base sheet, in **both**, with `D_8018FC8C`/`D_8018FC98` = 0 in both — the sheet switch did not fire between the two presses (a negative test, §4c) |
| all eight stores to `D_8018FC8C` in `asm/40E80.s` | init `0x801790B4`, `func_8017A740` set/clear `0x8017A77C`/`0x8017A858`, `func_8017AA38` clear/restore `0x8017AA6C`/`0x8017AAF4`, `func_8017B0B8` set/clear `0x8017B114`/`0x8017B484`, save/restore `0x8017A778` |
| `python3 tools/ogre_ram.py <dev dump> find-ptr 0x802FB590` | two hits, both the `G_SETTIMG` operand inside a display-list buffer, so the plane address is computed from a pointer and not stored as data |

No generated code and no probe were touched: `grep -rl probe RecompiledFuncs/
Bank*Funcs/ app/` is empty.

## 6. Files changed

| File | What |
|---|---|
| `docs/guides/sprite-mechanisms.md` | new: the sprite and text mechanism guide |
| `docs/notes-black-sprite.md` | §2 byte order rewritten; §3b record layout and the withdrawn claim; §3c the developer's per-pose detail; §4 open list |
| `tools/rdram.py` | `word`/`half`/`byte` read ROM order; `Dump.logical` transpose removed; docstring |
| `tools/ogre_ram.py` | `Image` accessors read ROM order; `find-ptr` big-endian; docstring |
| `docs/guides/app-build.md` | the "Reading a dump" section |
| `docs/README.md`, `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md` | the record |
| `docs/HANDOFF-2026-10-11-session132.md` | this file |

## 7. What the next session should do

Everything below needs a driven run, because the fault is in no capture on disk.

1. Capture the fault: `debug/black-sprite-capture.sh`, `1` pressed while the
   first (correct) head is up and `1` again at the black frame, then
   `grep -c 'BLACK DRAW' run.log` — **a capture with 0 is not the fault**, which
   has now been the failure mode three times. `OGRE_CAPTURE_PRESENT` yields black
   PPMs on this machine, so the screenshot has to come from the desktop.
2. Test §4c first, because it is the only mechanism that explains the symptom:
   - `python3 tools/rdram.py <ram-2-full.bin> half <timg-8+0x78>` for the record's
     code, then search that code in the table at `D_80190F74`
     (`tools/rdram.py <dump> half <D_80190F74> <n>`); absent means the `-1` path;
   - `python3 tools/rdram.py <dump> word 0x80190F60` and
     `word 0x8018FC8C` / `word 0x8018FC98` in both pose dumps: a working sheet
     that changes across the pose change, or a selector left at the wrong value,
     is the sheet switch firing.
3. If the record is populated and its code is in the table, the fault is not the
   `-1` path: then `tools/rdram.py diff` plus
   `tools/watch.sh <timg> --after <symbol>` names the writer of the buffer the
   draw actually samples.
4. Falsified already — do not re-run: cache corruption (`0x1FFF` is every
   record's initial code), cache churn under eviction pressure (35 of 256
   records in use), `timg` eight bytes low (the test read the upstream record's
   code field), and session 131's cell-plus-palette model (byte order).

## 8. Corrections to earlier sessions

- Session 130/131's `tools/ogre_ram.py`/`tools/rdram.py` readings are
  byte-reversed; every `find-ptr` hit, every `cell` rendering and the "cell +
  palette" model in `docs/HANDOFF-2026-10-11-session130.md` §3b and
  `-session131.md` §1 are affected. The correct record layout is §2 above.
- Session 130's "the mission field rebuilds the cache on every entry" stands: it
  is `func_80173724`, calling `func_801792DC` then `func_80179080`.
- Session 132's own first conclusion (the eight-byte `timg` fault) is withdrawn
  in §3.
