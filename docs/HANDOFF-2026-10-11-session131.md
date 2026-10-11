# Handoff — 2026-10-11, session 131: the bound part is all zero, the palette is not the fault

Session 130 ended on a developer dump: a sprite part was drawn black, and the
64 bytes at the address the draw bound (`0x8026A5C8`) were zero. It read those
bytes as the part's palette and left the next step as "find the routine that
fills the palettes and why this one is skipped".

**Result.** That reading is wrong. The zero bytes are the cell's **pixel data**;
the palette is the 32 non-zero bytes at cell `+0x60`. A second developer capture
on `scene_14.bin` (scene `0x14`, the Witch's Den, head bowed) confirms the cells
are the font cache's glyph cells: they render as letters, a blank cell decodes
to the transparent colour 0, and an all-zero rectangle is therefore **not** the
black part. The fault is not in the palette and not in those rectangles. The
capture and the reader for the next step are landed: a whole-image RDRAM dump on
a key press for every press, and `OGRE_TEX_TRACE_MS=0` + `OGRE_TEX_TRACE_KEY2`
so one run's trace covers the pose change by construction. No game run was made
by this session; the developer drives.

## 1b. Evidence from the developer's second capture

`/Users/momo/Desktop/ogre-battle-64-recomp/black-sprite/` (see
`docs/notes-black-sprite.md` for the full reading):

- the pool cells at `0x8026A4C8`, `0x80269CC8`, `0x80269C48` decode as a capital
  D, a lower-case u and a T fragment, so the 11x14 rectangles are the font cache
  and the all-zero ones are blank cells;
- every captured CI4 rectangle shares **one** palette (28 bytes, 27 non-zero),
  so no draw in the window had a missing palette;
- the whole-image dump holds three copies of the display list, all F3DEX2, and
  the character-sized draws in it are non-zero in every image the trace sampled.

**A byte-order correction that cost this session time.** The display lists in
the dump are not stored in the same byte order as the image data. Reading a list
word big-endian produced a plausible but wrong decode (wrong rectangle offsets,
wrong formats). The renderer's own walker is the authority, and
`OGRE_TEX_TRACE` is that walker's output; the hand-decode was withdrawn. The
same trap applies to `tools/rdram.py word` (`word` prints the guest word; a
display list must be read the way RT64's `texrect` reads it).

## 1. What the traced rectangles are

Session 130's `OGRE_TEX_TRACE` capture (`/tmp/black130/log4.txt`, `dump4/`) holds
59 textured rectangles in one display list of scene `0x0D`. Every rectangle is
11x14 screen pixels sampling a 12x15 texel window of a CI4 tile
(`fmt=2 siz=2 imgw=1`, tile `line=1`, `pal0`), and each binds a different
address in a contiguous pool starting at `0x802696C8`, one 12x16 cell at a time.
The app's own decoder says the same numbers (`dump4/tex/000008_src.json`:
`{"rdram":2532808,"count":34,"fmt":2,"siz":2,"line":1}`).

A 12x16 CI4 cell is 96 bytes. In the developer's dump
(`/tmp/black130/ram-1.bin`, base `0x802696C8`) each cell holds:

- `+0x00..0x5F` — the cell's 12x16 4bpp pixels, 8 bytes per row, only the low
  nibble of each byte used (row 0 of cell `0x8026A4C8` is
  `33 33 21 00` = `33332 1..`).
- `+0x60..0x7F` — a **16-entry RGBA5551 palette, present and non-zero** (at
  `0x8026A5C8`: `0000 0000 0000 0000 3313 1330 2313 1230 0111 1110 0000 0000`).

Rendered as the game's own format, `0x8026A4C8` reads as a capital letter and
`0x8026A648` reads as another. These are the text/bitmap cells of session 130's
font cache, and the 59 draws are one line of dialogue. `tools/ogre_ram.py cell
<image> <addr>` prints a cell this way.

## 2. The black rectangles

`dump4` (a *different* run from the dump, so the two are only comparable inside
themselves) has 12 rectangles whose source bytes are all zero. The tool marks
them `NO OPAQUE TEXEL in the sampled window   <-- BLACK`. Their bound addresses
are `0x8026A5C8`, `0x802697C8` (three times), `0x8026A6C8`, `0x802696C8`,
`0x802698C8`, `0x80269B48`, `0x80269BC8`, `0x8026A748`, `0x80269948`,
`0x80269EC8`, `0x8026A3C8`.

The bound-and-read bytes are zero. That is the whole fault: a draw samples
palette index 0 in all 34 of the bytes it reads, and colour 0 of the bound
palette renders black. The palette is not empty and was not "never written".

In the developer's separate dump the same cell (`0x8026A5C8`) also has zero
bytes at `+0x00`, and non-zero bytes only from `+0x40`; cell `0x8026A4C8`, bound
in the same list, has data from `+0x00`. One cell holds its pixels, its
neighbour does not, in the same frame and the same pool.

`dump4/tex/` is the ground truth for this, not a reconstruction: the app wrote
those bytes out on the frame it drew them.

## 3. The sprite-part engine, at instruction level

The engine is a task-queue producer, and `tools/symbols.md` already names its
machinery: `func_800765D8` copies each slot of `0x800E7A30` and `jalr`s the
callback.

- `func_ovlA_801A07F0` (`bankRec3`, ROM `0xEBBD0 + 0x1980`, unit A) is the part
  entry. It `bzero`s and fills a 0x160-byte work block reached through
  `*(0x801BBA78)`, sets the callback slots `0x800E7A40/44/48/4C` and the record
  counts `0x800E7A50/54`, and copies a 0x54-byte code/data template from
  `0x801BB980` to `0x801BBA80` (`0x801A08D4..0x801A0910`, `0x801A0928..0x801A095C`).
- It indexes a **12-byte-per-entry table at `0x8019F830`** with the byte
  `0x800E7AB8`: `id*12 + 0x8019F830`, read as `lw -0x7D0` (`0x801A0BC4..0x801A0BE4`).
  That is the part's code pointer; the entries also carry a flag that selects an
  asset variant by `id` (`0x801A0AFC..0x801A0BAC`, `0x801A0AD0`).
- It pulls the part's asset with `jal 0x8007938C(a0 = 0x1E39A52, a1 = code
  pointer, a2 = -6)`, then reads the returned block's first words as **relative**
  pointers: `asset[0]`, `[1]`, `[2]` are added to the block (`0x801A0C24..0x801A0C4C`),
  and `asset[+0x30]` aligned to 16 is added for another section
  (`0x801A0C50..0x801A0C64`). Asset `0x1E39A52` = ROM `0x7175A2`.
- `func_ovlA_8007938C` is the game's own code-list interpreter: the block at
  `sp+0x3F & ~3` is a length, and lengths `< -23` run inline code lists
  (`0x800793EC..0x80079440`, jump table `0x800BDFA8`). The port compiles its own
  instructions, so this is not a port-specific reader.
- Each frame is decompressed by `jal 0x8007A110` into a destination that
  **accumulates**: `a0 = s4 + s1`, then `s1 += return value`
  (`0x801A0E24..0x801A0E5C`). `s4` is `func_80070F30`'s block, aligned 16.
  One frame's size therefore shifts every later frame.

## 4. What is open

The mechanism inside that engine is not yet named. The two candidates the
evidence now separates:

- the part's asset resolves to a block whose frame pointer is right and whose
  decompressed bytes land at a different offset than the draw reads (the
  accumulating `s1` in §3), so every later frame in the part is one frame's
  worth out; or
- the pool cell that the draw binds is one cell earlier than the cell the
  engine filled, so the draw reads a cell that is still zero while the filled
  one holds the pixels.

Both are write-side faults and neither is visible from a single window dump.
The next artefact is one **full 8 MiB RDRAM image taken at the black frame on
the world-map route**, plus the screenshot of that frame.

## 5. What was built

- `app/src/ram_key.cpp`: `OGRE_RAM_KEY_FULL=1` writes the whole image to
  `<OGRE_PREF_DIR>/ram-<n>-full.bin` instead of the `_ADDR`/`_LEN` window, on
  the same key press. The window mode is unchanged. **Every press now writes
  another dump** (`was_down` state instead of `console::key_pressed`), so one run
  can take `ram-1-full.bin` and `ram-2-full.bin` either side of a pose change;
  `console::key_down` is new for that.
- `tools/ogre_ram.py` (new): reads such an image with the runtime's byte order
  (`tools/rdram.py`'s rules) and answers the two questions a window cannot.
  - `find-ptr <addr>` — every word in the image equal to that address, with its
    neighbours, so the table or display list that chose the buffer is named.
  - `cell <addr>` — one slot printed as the game's own CI4 image and palette,
    with `nonZero`/`mean` and `first non-zero at`, and `--png`.
  - `empty-cells <lo> <hi>` — which slots in a range are all zero.
- `docs/guides/app-build.md`: the `OGRE_RAM_KEY` row carries `_FULL` and the new
  tool.
- `debug/black-sprite-capture.sh` (new): one command for the capture run. It
  loads a battery image with `OGRE_SAVE` (`OGRE_SAVE_RESET=1`, so every run
  starts from the same file), puts the whole preference directory under
  `<outdir>/pref`, arms `OGRE_RAM_KEY=1 OGRE_RAM_KEY_FULL=1` and
  `OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_KEY=2 OGRE_TEX_TRACE_KEY2=3
  OGRE_TEX_TRACE_MS=0 OGRE_TEX_TRACE_DUMP=<outdir>/texdump`, and tees the run to
  `<outdir>/run.log`. `MS=0` holds the trace from the press to the window close,
  so the faulty frame is inside the capture by construction. The default save is
  the developer's `~/Desktop/ogre-battle-64-recomp/saves/scene_14.bin` and the
  default outdir is beside it. `OGRE_SPEED=1` on the command line plays at normal
  speed.
- `app/src/renderer.cpp`: `OGRE_TEX_TRACE_KEY2`, a second arming key on its own
  edge state (`console::key_pressed`'s new `slot`), so a press that was too early
  does not consume the next one.

## 5b. A freeze this session caused, and the guards

The first version of the colour sampler read RDRAM with no bounds check. The
developer's run hung, and its log ends:

```
[renderer]   processDisplayLists 38045008 us (dl 3042)
```

38 seconds inside one display list, with the trace armed and colour on. The
sampler now checks every read against RDRAM, caps one draw at 65536 texels, and
reports a black draw only when at least 32 of its texels are opaque (the earlier
threshold flagged one-pixel rules). The dump path also stops after 4000 file
pairs with a `[tex] dump limit` line, because a capture that runs to the window
close otherwise writes two files per rectangle.

Verified after the guards: a title-scene run with `OGRE_TEX_TRACE_COLOUR=1
OGRE_TEX_TRACE_MS=0` reports 9471 `colour:` lines, **0** `BLACK DRAW` lines and no
stall (`processDisplayLists` stays in the milliseconds); the same run before the
opaque floor produced 9 false `BLACK DRAW` lines. `debug/black-sprite-capture.sh`
no longer sets `OGRE_TEX_TRACE_DUMP`, so a long capture writes one log.

## 5c. Every run is launched with no audio

`app/src/sdl_platform.cpp` logs `[SDL] OGRE_NO_AUDIO: no output device opened`
when it skips the device, so a run says which path it took.
`debug/black-sprite-capture.sh` sets `OGRE_NO_AUDIO=1`, and AGENTS.md rule 14
makes it the rule for every run of the game.

## 6. What was run for verification

| run | evidence |
|---|---|
| `python3 tools/ogre_ram.py /private/tmp/cur-b.bin cell 0x8027A5C8` | a full image the port wrote earlier decodes as 16 rows of 16 CI4 texels with `nonZero 218/256`; the reader's byte order matches the display list |
| `python3 tools/ogre_ram.py /private/tmp/cur-b.bin find-ptr 0x8027A5C8` | two hits, `0x803025C4` and `0x80352FE4`; both are `FD100000 8027A5C8`, i.e. the `G_SETTIMG` operand of a display list, so the search finds live structures |
| `python3 tools/ogre_tex_uv.py dump4 --log log4.txt` | 12 rectangles with `NO OPAQUE TEXEL in the sampled window`; every other CI4 cell in the same list has data |
| reading `/tmp/black130/ram-1.bin` at `0x8026A5C8` and `0x8026A4C8` | one cell's pixels are zero and its palette present; the neighbouring cell bound in the same list is populated |
| `OGRE_SCENE=title OGRE_RAM_KEY=9 OGRE_RAM_KEY_FULL=1 OGRE_RAM_KEY_AT_MS=8000 OGRE_EXIT_AFTER_MS=10000 ./build-dist/ogrebattle64` | `[ramkey] key 9: wrote /tmp/s131run/ram-1-full.bin (8388608 bytes, whole image)`, exit 0 |
| `python3 tools/ogre_ram.py /tmp/s131run/ram-1-full.bin find-ptr 0x80000000` | hits at `0x800A8870`, `0x800AC96C`, `0x800C6890` with the neighbouring words, so the reader and its byte order work on a full image |
| `python3 tools/ogre_ram.py /tmp/s131run/ram-1-full.bin empty-cells 0x80268000 0x80270000` | `0x8026B100`, `0x8026B200`, `0x8026B300`, `0x8026B500` are all zero on the title, which is the same pool shape the cutscene has |
| `make app` | the knob and the tool build. **`make app` wrote `build-dist/ogrebattle64`**, not `build-app/ogrebattle64`: `DIST_STATIC_SDL` was set in the environment of that build, so it uses `APP_BUILD_DIR := build-dist` |
| `bash debug/black-sprite-capture.sh` (twice, `OGRE_EXIT_AFTER_MS=4000`) | the script loads `scene_14.bin` (`[save] OGRE_SAVE=…: a battery image (logical)`), writes `<outdir>/pref`, tees `run.log`, and reports `no full-RDRAM dump: key 1 was not pressed` and `texture dump files: 0` when no key was pressed, which is the correct empty result |

No generated code was touched, so no probe needed reverting: `grep -rl probe`
finds nothing in `RecompiledFuncs/`, `Bank*Funcs/` or `app/`.

## 7. Files changed

| File | What |
|---|---|
| `app/src/ram_key.cpp`, `app/src/ram_key.hpp` | `OGRE_RAM_KEY_FULL` |
| `tools/ogre_ram.py` | new: find-ptr / cell / empty-cells for a full image |
| `docs/guides/app-build.md` | the `OGRE_RAM_KEY` row and the tool |
| `PLAN.md`, `docs/STATUS-LOG.md`, this file | the record |

## 8. The run to ask for

The developer reached the fault with `scene_14.bin` (scene `0x14`, the Witch's
Den), where the Old Witch's portrait is the black part. One command:

```
debug/black-sprite-capture.sh
```

or explicitly:

```
debug/black-sprite-capture.sh \
  build-dist/ogrebattle64 \
  ~/Desktop/ogre-battle-64-recomp/saves/scene_14.bin \
  ~/Desktop/ogre-battle-64-recomp/black-sprite
```

Press `1` just before the part turns black (the good state), `2` at the moment
it is black (the trace, which runs to the window close), and `1` again after it
(the bad state). The two full images are the same buffers in both states. Add
`OGRE_SPEED=1` to the command line to play at normal speed. The screenshot is
what names which rectangle is the black part; the `[tex]` lines carry the
rectangle, so the two correlate directly.
