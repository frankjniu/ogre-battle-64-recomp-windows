# Handoff — 2026-10-11, session 130: a sprite part that draws black

Developer report: *"there's a bug that happens sometimes during cutscenes. in this
game, the character's sprites are composed of multiple parts. in the example
screenshot, the knight's head was replaced with a black texture... it's an issue
that happens in about 10% of cutscenes"*, and: *"it happens in specific cutscenes,
but only if I navigate from the world map into them. for example, there's a
pre_ogre save at ~/Desktop/ogre-battle-64-recomp/saves. it suspend data has a
save just before a cutscene during a mission... if I load the suspend data, the
issue doesn't happen. but if I load the save at the world map, move into the
mission and trigger the cutscene, it does happen."* Behaviour: the black part is
**static for the whole shot**, and the character is a **2D sprite over a
pre-rendered background**.

The shape is therefore a **path-dependent, per-part, static fault**: the same
character and the same shot draw correctly from one route and draw one part
solid black from another. No prior session recorded it.

**Result.** The investigation is at the point where one capture decides the
mechanism, and the capture tooling is landed and validated. Two leads are closed
and one is opened. The developer offered to drive the game, so no game run was
made for the bug itself.

## 1. What was closed

**The sprite/bitmap cache at `0x8017909C` is not the character sprite system.**
The developer's description ("composed of multiple parts") pointed at
`func_8017909C` (`asm/40E80.s:17123`, 230 instructions), which loads four assets
into one block, lays out 256 `0x80`-byte entries, and publishes a set of pointers
(`D_8018FC88` block base, `D_8018FC90`/`D_8018FC94` section bases, `D_80190F60`
`/64/68/6C/70/74`). It is the **font/bitmap-text cache**: its consumers are the
text layout and text renderer functions, and the byte its accessor returns is a
per-character **advance/width** that is summed into a pen position
(`func_801794E8` `addu $s0,$s0,$a0` at `0x801798A0`; `func_80179F24`'s pen
advance at `0x80179ED4`-`0x80179EF0`). The two 42 KiB assets it holds
(`0x218C0D4` and `0x218DACC`, ROM `0x2720324` and `0x2721D1C`, both `0xA600`
bytes) are glyph sheets. A zero byte there is a zero advance, which shifts text,
and not a black sprite part.

This is worth recording because it is the shape the report suggests: the loads
are six scenes' worth of setup and the free/load pairs are not symmetric, so it
looks like a stale-cache bug. It is not this one.

**Corrections to the first reading of `func_8017909C`** (all from the
disassembly; they matter because the block is also the part cache).

- Three assets are decompressed into the malloc'd block, and the fourth value is
  a **ROM address**, not a copy: `D_80190F60 = func_8009DD28(0x218C17E)`, and
  `func_8009DD28` is `return a0 + 0x594254` (`asm/1060.s:53184`), so
  `D_80190F60` = ROM `0x27103D2`, the compressed part/glyph sheet.
- `D_80190F70` (`= block + 0x8000`, `0x801791CC`) is **not** a section base: it is
  bzeroed for `0x200` bytes (`0x801791D4`) and used as a **presence bitmap over
  codes `0x000..0xFFF`** (`*(u8*)(D_80190F70 + (code>>3))`, bit `code&7`). The
  section bases are `D_8018FC90 = D_80190F70 + 0x200` (asset `0x218C0D4`),
  `D_8018FC94 = D_8018FC90 + align8(sizeA)` (asset `0x218DACC`) and
  `D_80190F74 = D_8018FC94 + align8(sizeB)` (asset `0x216C662`, a `u16` table).
- The slot table is 256 entries of `0x80` bytes: `+0x00` prev, `+0x04` next,
  `+0x78` the code (`0x1FFF` initially), `+0x7A` a flag, and the **part bitmap is
  the slot's `+0x08` payload** (`0x70` bytes, 4bpp).

**The same block is a runtime part cache, and a miss there is a black tile.**
`func_8017A908` tests the presence bitmap for the part's code and calls
`func_8017AE40`, which DMAs the part out of the ROM sheet (`D_80190F60 +
code*0x138` for codes `< 0x100` or `>= 0x200`, `D_80190F60 + (code-0x100)*0x10`
for `0x100..0x1FF`, `0x8017AF38`-`0x8017AF98`) and expands it into the slot
payload. `func_8017A740` resolves a code by binary-searching the `u16` table at
`D_80190F74` and returns `-1` when the code is absent. `func_801A8124` then
**bzeroes the `0x70`-byte part bitmap** (`0x801A81FC` -> `func_80093380(sp+0x128,
0x70)`), which is palette index 0 in every pixel. So one missing part code is one
black tile, and the sheet selector `D_80190F8D` is reset to 0 by
`func_80179374` on every flush, so the script has to re-issue the sheet command.

**The mission field is not drawing through the world map's cache.** Scene `0x03`'s
own descriptor word 0 is `func_80173724`, which calls `func_801792DC` (free) and
then `func_80179080` (load) at `0x8017372C`/`0x80173734`, so every entry into the
mission rebuilds the cache. Scene `0x0D`'s enter `func_80178568` has neither
call, but the New Game chain reaches it through scene `0x02`, whose enter
`func_80178920` -> unit E `func_80198D28` -> `func_80178D30` -> the loader, so
`0x0D` is never drawing on a table left by scene `0x05` either.

**A conflict with the report that the trace has to settle.** The small figures
standing in scene `0x0D`'s cathedral and hall shots are **baked into the njpeg
backdrop**, not drawn as sprites: `docs/proofs/native-newgame-cathedral-background.png`
already holds the soldiers and `-cathedral.png` adds only the window, the portrait
and the text. The 2D part compositor that exists (`func_801A8124` ->
`func_801A7FA4`, which blits 11x14 4bpp tiles and emits no display list) is reached
from scene `0x12` (Load Game)'s update `func_80178430`, and no static path was
found from scenes `0x03`, `0x05` or `0x07` to it. So either the screenshot's scene
is not `0x0D`, or the sprite is drawn by a path not yet identified. The trace
answers this without needing the scene to be named first.

## 2. What was opened

The world-map route and the suspend-load route differ in the **heap and scene
history before the mission**, and the mission's own enter is identical. So the
fault is in state the map route leaves behind, or in a table the mission fills
from data that route changes. A black part is consistent with a buffer whose
composite step did not run or wrote `0` (as the map's own knight sheet does: the
map's enter `malloc(0x18000)`s `state[+0x34]` and composites an RGB555 LUT over
an 8-bit index image into it).

## 3. What was built

`app/src/renderer.cpp` gained an `OGRE_TEX_TRACE` pass
(`namespace tex_trace`, ~line 440; armed in `send_dl` beside `OGRE_NOP_RECT`). It
walks the submitted display list before RT64 parses it, tracking `SETTIMG`
(format/size/width/address), `SETTILE` t0 (line/palette), `SETTILESIZE` t0 and
`LOADTLUT` t0, and prints one `[tex]` line per `G_TEXRECT`:

```
[tex] dl=477 n=0 TEXRECT rect=124,0..196,0 px=72x0 s=0 t=4608 dsdx=1024 dtdy=1024
      window=uls0 ult0 lrs316 lrt200 tile=line5 pal0 timg=0x801C2778
      fmt=2 siz=1 imgw=40 texels=73x1
```

`OGRE_TEX_TRACE_DUMP=<dir>` also writes the RDRAM the draw reads
(`<dir>/tex/<n>_src.bin`, plus `_pal.bin` for an indexed image) with a `.json`,
and prints a byte summary per draw (`[tex]   src: raw=… nonZero=… mean=…
distinct=… first16=…`). `OGRE_TEX_TRACE_AFTER_MS=<n>` stays silent until the
run's trace clock passes `n` ms, so one bad frame is a small capture;
`_DRAW=<n>` reports a single rectangle, `_MAX=<n>` caps the count. Nothing is
patched, so a trace run is the game's own display list.

`tools/ogre_tex_uv.py <dir>` decodes that dump: a per-draw summary with the mean
and alpha of the texels the rectangle's window covers (the sampled window, and
the tile's declared window), `--dark` for only the black ones, `--png` for a
picture of each dumped image. It reads the log (`<dir>/log.txt` by default, or
`--log`) and the `tex/*.json` metadata, and handles 4b/8b (through the dumped
TLUT), 16b and 32b.

The line format is **validated against the app's own decoder**: `OGRE_DL_DECODE`
prints `TEXRECT ulx/uly/lrx/lry s t dsdx dtdy timg fmt siz w`, and the trace
reproduces the same numbers for the same rectangles on a boot run
(`ulx=496`/`lrx=784` vs `rect=124,0..196,0`, `s=0 t=4608`, `timg=0x801C2778
fmt=2 siz=1`, with the app printing `w=40` where the raw field is `39`, which is
the `+1` convention and is now what the trace prints). `G_TEXRECT`'s word order
was read from `app/src/gbi.cpp`'s `OP_TEXRECT`/`OP_RDPHALF_1`/`OP_RDPHALF_2`
cases: `+0` lower-right, `+4` upper-left (both in quarter pixels, x in bits
12..23, y in bits 0..11), `+8`/`+16` the two `RDPHALF` commands whose second
words carry `s`/`t` and `dsdx`/`dtdy`.

Zero-height rectangles (`uly=0 lry=0`) are real and appear in the boot logos'
font draws; they are the app's own decode of the same list, not a parsing error.

## 3b. The fault is a palette that is never written (the developer's dump)

`OGRE_RAM_KEY` (new: `app/src/ram_key.{hpp,cpp}`, ticked from
`sdl_platform.cpp`) writes the guest RDRAM at `OGRE_RAM_KEY_ADDR` to
`<OGRE_PREF_DIR>/ram-<n>.bin` and `.txt` on a digit key press, so a bug can be
investigated without copying anything out of a terminal. `OGRE_RAM_KEY_LEN`
(default 0x1000), `_OUT`, `_STRIDE` (default 0x80, the grid the battle's part
palettes sit on) and `_AT_MS` (a scripted-run spelling) complete it.

The developer's dump of `0x802696C8`, 0x1400 bytes, taken at the moment the head
is black, settles the mechanism. The image the character's head binds is
`0x8026A5C8` (`fmt=2` CI, 64 bytes = a 32-entry RGBA16 palette), and **its first
64 bytes are all zero**:

```
first 64 bytes: 0000...0000   (all 64)
non-zero byte offsets in the 0x80 slot: [72..75, 80..83, 88..91, 113..127]
```

The non-zero bytes start at +72, i.e. **past the 64-byte palette**, so they
belong to the following entry. The palette itself has never been written. Every
pixel that indexes it reads colour 0, which is the black head.

The same dump shows the neighbouring slots populated (`nonZero 24..55/128`,
`mean 8.9..19.0`), and a search of the whole cartridge for the first 32 non-zero
bytes of each populated slot returns **no exact match**, so these palettes are
not copied verbatim from ROM; they are computed or assembled at load time. The
next step is the routine that fills them and why this one is skipped — the
address and the empty bytes are now pinned.

This also explains why the earlier `OGRE_TEX_TRACE` capture missed it: that run's
window contained only the cutscene's UI display list, so the `SETTIMG` of the
character's part images was never in it. The image report and the RAM dump are
the two halves that found the fault.

## 4. What is needed to finish

One developer-driven run of the world-map route into the bad cutscene with the
trace on, pressing the armed digit key while the black part is on screen. From
the `[tex]` line for the black part the source address is known; from the dump,
whether the bytes there are black decides between:

- **the source is black** — the buffer the part reads (or its palette) was not
  filled, i.e. the composite step that should have written it did not run on that
  route; and
- **the source is fine** — the rectangle reads the wrong place (a bad address or
  window), i.e. a pointer/table entry differs by route.

A screenshot of the bad frame is needed alongside it, to name which rectangle is
the black part. The `[tex]` lines carry the on-screen rectangle, so the two
correlate directly.

**The part cache is the other candidate, and it produces a black tile by
construction.** `func_8017A740` returns `-1` when the part's code is absent from
the `u16` table at `D_80190F74`, and `func_801A8124` then bzeroes the `0x70`-byte
part bitmap (`0x801A81FC`), which is palette index 0 in every pixel. The same
zero-fill runs when the getter `func_8017A908` returns 0. So "one part is black"
is the designed result of a code that does not resolve, and the ranked places for
that to go wrong are `D_80190F74` (set from the block layout at `0x8017929C`),
the sheet id `D_80190F8D` (written from the message script at `0x8017B3C4`, reset
to 0 by `func_80179374`) and the presence bitmap `D_80190F70` (set at
`0x801791D0`; `func_801792DC` frees the block at `0x801792F4` without clearing
it). That compositor is reached from scene `0x12`'s update `func_80178430`, and
no static path was found to it from scenes `0x03`, `0x05` or `0x07`, so the
screenshot may also show that the bad sprite has a different origin.

## 5. What was run for verification

| run | evidence |
|---|---|
| `/tmp/textrace-validate2`, `/tmp/tt3`, `/tmp/tt4`, `/tmp/tt5` | the trace against the app's own `OGRE_DL_DECODE=all` output on the same boot: matching `s`/`t`/`timg`/`fmt`/`siz`/width and matching quarter-pixel rectangles; 40 rectangles per display list on the boot logos; `armed at t=9016ms` with 12 reported rectangles and 12 dumped files when `OGRE_TEX_TRACE_AFTER_MS=9000` |
| `/tmp/run130`, `/tmp/run130b` | two attempts to drive the mission from `assets/scene_5_suspend.bin` with the documented tap recipe. **Both discarded**, and they are why this session asked the developer to drive instead: the app on this machine reaches 5.9 s of game time in 70 s of wall clock with `OGRE_DL_TRACE` on (3 display lists, `processDisplayLists` 417 ms for one of them), and even without it the title is not reached before the tap window (`[scene] t=9713ms id=0x0004`, taps consumed, exit at 120 s). A scripted route is not a usable instrument for this bug on this machine |

## 6. Files changed

| File | What |
|---|---|
| `app/src/renderer.cpp` | the `OGRE_TEX_TRACE` pass (`namespace tex_trace`) and its arming in `send_dl` |
| `tools/ogre_tex_uv.py` | new: decodes the trace's dumps into a per-draw summary, `--dark`, `--png` |
| `docs/guides/app-build.md` | the `OGRE_TEX_TRACE` knob row |
| `docs/DECISIONS.md`, `PLAN.md`, `docs/STATUS-LOG.md` | the record |

No generated bank code changed; no probe was left in `RecompiledFuncs/`,
`Bank*Funcs/` or `app/`. `git status --short` shows only `app/src/renderer.cpp`,
`docs/guides/app-build.md`, the new `tools/ogre_tex_uv.py`, and the pre-existing
`tools/RT64` modification.

## 7. Knobs

| variable | what |
|---|---|
| `OGRE_TEX_TRACE=1` | one `[tex]` line per textured rectangle, plus the byte summary |
| `OGRE_TEX_TRACE_KEY=<1..9>` | arm the capture when that digit key is pressed (edge-triggered, read at display-list submission via `console::key_pressed`). **This is the intended way to capture a bad frame**: press the key while the frame is on screen. Each press re-arms, so several captures can be taken in one run |
| `OGRE_TEX_TRACE_MS=<n>` | how long the capture stays armed after it starts (default 2000; 0 = until exit). The capture closes by itself and says so |
| `OGRE_TEX_TRACE_DUMP=<dir>` | write the sampled RDRAM to `<dir>/tex/<n>_src.bin` (+ `_pal.bin`, `.json`); the directory is created on demand |
| `OGRE_TEX_TRACE_AFTER_MS=<n>` | the timer alternative to the key: arm once when the run's trace clock passes `n` ms (one-shot, does not repeat) |
| `OGRE_TEX_TRACE_DRAW=<n>` | report only the nth rectangle of the run |
| `OGRE_TEX_TRACE_MAX=<n>` | cap the reported rectangles (default 20000) |
| `tools/ogre_tex_uv.py <dir> [--log F] [--dark] [--top N] [--png]` | decode a dump; `--top N` prints the N darkest samples, which is the fast way to find the black part |
| `OGRE_RAM_KEY=<1..9>` (`_ADDR`, `_LEN`, `_OUT`, `_STRIDE`, `_AT_MS`) | press that key and the run writes the guest RDRAM at `_ADDR` (default `0x802696C8`) to `<OGRE_PREF_DIR>/ram-<n>.bin` and `.txt`, the `.txt` ending in a per-slot `nonZero`/`mean` summary that marks an empty slot. `_LEN` default `0x1000`, `_STRIDE` default `0x80`. Read-only |
| `OGRE_SCENE=title` | boot straight to the title scene `0x04` instead of waiting out the boot intro and publisher stills (verified: `forcing scene 0x04` / `forced scene 0x04 is active` at `t=3793ms`) |

## 8. Open

- The mechanism of the black part, for the reason in §4.
- Whether the two routes differ in the **heap** (the map allocates and frees
  large blocks; the mission's enter then allocates its own) or in a **table the
  mission fills** from data the route changes. A memory dump of the composite
  input on both routes separates these, and the live console's `dump` command can
  take it at a named scene and step (`OGRE_CONSOLE_ON_SCENE`,
  `OGRE_CONSOLE_ON_STEP`, `OGRE_CONSOLE_ON_CMD`).
- The character sprite composite itself has not been located. The map's is
  `func_ovlM_801A2A7C`'s two `sprite_rect_emit` calls plus the enter's
  `malloc(0x18000)` composite (`docs/HANDOFF-2026-09-17-session64.md` §2); the
  mission's 2D layer consumes the font cache (`func_ovlN_801E09FC` and
  `func_ovlN_801E0DDC` call `func_80179404` at `0x801E0A58`, `0x801E0AB4`,
  `0x801E0B0C`, `0x801E0C1C`, `0x801E0E1C`, `0x801E0E6C`) and its character art
  has not been traced to a buffer.
