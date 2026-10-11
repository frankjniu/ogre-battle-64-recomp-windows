# The black sprite part: what the captures show

Session 131. Developer state: `scene_14.bin` (scene `0x14`, the Witch's Den),
character sprite with the head bowed. Two captures, both in the developer's
`~/Desktop/ogre-battle-64-recomp/black-sprite/`.

* 11:06 — `ram-1-full.bin` (whole 8 MiB RDRAM, one press) and a 4000 ms
  `OGRE_TEX_TRACE` window. The developer reported the head black at that moment
  and sent a screenshot of it.
* 11:21 — `ram-1-full.bin`, `ram-2-full.bin` (two presses) and a trace that ran
  from the press to the window close. The developer reports the head rendered
  correctly the whole time.

**Status: not reproduced in the second capture, and not fixed.** The evidence
below says the condition that was present when the head was black is also
present in the run where it rendered correctly, so the two cannot be separated
from these byte images. The bug is reported at about 10% of cutscenes.

**What the next run adds (`OGRE_TEX_TRACE_COLOUR=1`).** Both captures could only
report the *bytes a draw samples*. A draw that samples a correct cutout and
paints it black samples non-zero bytes, so the digest looked healthy. The trace
now decodes each sampled window **through its palette** and prints the colour
mean, and prints one `[tex] BLACK DRAW: …` line the first time a draw's mean is
at or below `OGRE_TEX_TRACE_BLACK` (default 4 of 255) **and at least 32 opaque
texels**. A capture of the black
frame will therefore name the faulty draw in the log, and a capture with **no**
`BLACK DRAW` line would say the black part is not a `G_TEXRECT` at all (a
textured triangle or an S2DEX object). Verified on a title-scene run: the trace
prints `[tex]   colour: mean 29.2/255 over 321x7 texels, opaque 2247` and
friends, tracking a fade.

The dump's byte order is the runtime's: the guest word at `a` is
`struct.unpack_from('<I', image, a & 0x1FFFFFFF)`, and the logical byte at `a` is
`image[(a & 0x1FFFFFFF) ^ 3]`. `tools/ogre_ram.py` / `tools/rdram.py` use those
rules.

## 1. What the trace captured

The 11:06 window held display list 3273 and its copies: 59 textured rectangles,
55 of them 11x14 over a 12x15 texel window, plus three full-width strips and two
16x16 icons. Per-draw digest of every captured image (`texdump/tex/NNNNNN_src.bin`,
with the palette the `LOADTLUT` before it loaded):

* every 11x14 rectangle binds a CI4 tile (`fmt=2`, `line=1`) and shares **one**
  palette (`_pal.bin`, 28 bytes, 27 non-zero: `2fe7 8d4a dfbd 9fb5 …`);
* 12 of the 55 bind a tile whose sampled bytes are **all zero**;
* the character-sized rectangles (draw 0, `36,19..294,80`; draw 3,
  `36,24..76,72`) and the two icons are `fmt=0 siz=2` and non-zero.

Rendering the pool cells at 12x16 CI4 (8 bytes a row, logical bytes) shows
**letters**: `0x8026A4C8` is a capital D, `0x80269CC8` a lower-case u,
`0x80269C48` a T fragment. The 11x14 rectangles are the font cache's glyph
cells (session 130's finding), one per character of dialogue, and the all-zero
ones are blank cells. A blank cell decodes to palette index 0, whose colour 0 is
the transparent entry, so it renders as nothing.

Draw 8 of that window (`rect=88,41..99,55`, tile `0x8026A5C8`) is the one
session 130 followed: `texdump/tex/000008_src.bin` is 34 zero bytes.

## 2. The byte order, and the mistake it caused

The image holds display lists, and **a list is not stored in the same byte order
as the image data**:

* the list word at `0x802B5F30` has raw bytes `fd 10 00 00`, which is
  `G_SETTIMG fmt=0 siz=2 w=1 img=0x802FB590` (`json img = 0x802FB590`);
* `0x802B5F70` has raw bytes `e4 49 81 40 00 09 00 4c`, a `G_TEXRECT`.

Reading those words **big-endian** (which this session did first) makes them
`0xFD100000`/`0xE4498140` and produces a plausible but wrong decode: every
rectangle offset, the format of every image, and the frames. The authority is
the renderer's own walker, which reads them the way RT64's `texrect` does.
`OGRE_TEX_TRACE` is that walker's output. **Do not hand-decode a display list
with a different byte order than the renderer uses.**

The images are read with the logical byte at `a` at
`image[(a & 0x1FFFFFFF) ^ 3]` (the same rule the trace's dump uses); rendering
the strips at `0x802FB590` and `0x802FD700` that way gives a mid-tone gradient
rather than a picture, which says those planes are not a plain image at the
bound address: they contain **pointers** (`80 b3 2f 80` = `0x802FB380`) at
`+0x20`, `+0x30`, `+0x40`. Whatever the character is drawn from, it is not
those bytes taken as one flat texture.

## 3. The second capture (11:21)

`ram-1-full.bin` and `ram-2-full.bin` are the two pose presses, and the trace
ran from `t=45326ms` to the window close (`holding 0ms`): 10506 reported
rectangles over 238 dumped images in two display lists (4856 and 5021).

* The same two draws still sample **all-zero** bytes: `draw 8`, tile
  `0x8026A5C8`, rect `88,41..99,55`, and `draw 59`, the same tile, rect
  `157,198..168,212`.
* The cell at `0x8026A5C8` is **identical in both dumps**: `nonZero 12/0x60`,
  first non-zero at `+72`, which is the part of the 12x16 tile below the 34
  bytes the draw reads. That is the same state the black-head capture had.
* The character-sized draws (0–3, 57, 58, 61) are `fmt=0 siz=2` and non-zero.

So the run where the head was correct has the same bound tile, the same
all-zero window and the same cell bytes as the run where it was black. This
capture therefore does not show the fault, and cannot show a fix.

## 4. What is open

1. **Whether the black head is drawn from the 11x14 font cells at all.** The
   developer's description (a part of a multi-part character, correct cutout,
   black fill) fits a font cell whose palette decodes to black, and the pool is
   shared between text and parts; the alternative is a draw the `G_TEXRECT`
   walker does not report — a textured triangle or an S2DEX
   `G_OBJ_RECTANGLE*`. The 10506 reported rectangles contain no character plane
   beyond the six listed above.
2. **The difference between the two states.** It is not in the sprite pool (9
   changed bytes in `0x80260000..0x80300000` between the two dumps). Between
   the two presses the largest changed region is `0x80110000..0x8014FFFF`
   (about 39 KB), which is not where the sprite parts live.

## 6. The colour report (new)

`OGRE_TEX_TRACE_COLOUR=1` decodes each sampled window through its palette (CI4
and CI8 through the `LOADTLUT` address, RGBA16 directly) and prints

```
[tex]   colour: mean 29.2/255 over 321x7 texels, opaque 2247
```

and, the first time a draw's mean is at or below `OGRE_TEX_TRACE_BLACK`
(default 4, and at least 32 opaque texels):

```
[tex] BLACK DRAW: dl=3273 n=8 rect=88,41..99,55 timg=0x8026A5C8 fmt=2 line=1 tlut=yes
```

The point is that a part which samples a correct cutout and paints it black has
**non-zero source bytes** and a **black mean**, which is the combination neither
earlier capture could report.

Verified: a title-scene run with `OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_COLOUR=1
OGRE_SCENE=title` prints `[tex] armed at t=0ms (timer), holding 2000ms` and a
run of `colour:` lines tracking the fade (`mean 29.2` up through `mean 135.5`).
With `OGRE_TEX_TRACE_KEY=2` and no key pressed nothing is armed, which is the
documented behaviour.

`debug/black-sprite-capture.sh` sets `_COLOUR=1` and `_BLACK=4`.

**A freeze this cost.** The first version of the sampler read RDRAM with no
bounds check, and the developer's run hung: the log ends
`processDisplayLists 38045008 us (dl 3042)` — 38 seconds inside one display
list, with the trace armed and colour on. The sampler now checks every read
against RDRAM, caps a draw at 65536 texels, and the dump path stops after 4000
file pairs with a `[tex] dump limit` line (a capture that runs to the window
close otherwise writes tens of thousands of files). A title-scene run with
`_COLOUR=1 _MS=0` now reports 9471 `colour:` lines, **0** `BLACK DRAW` lines and
no stall; the same run before the guards produced 9 false `BLACK DRAW` lines on
one-pixel rules, which is why the 32-opaque-texel floor exists.

## 7. The next run

```
debug/black-sprite-capture.sh
```

Wire the keys to the fault in this order:

1. `1` **just before** the part turns black — writes the good state to
   `ram-1-full.bin`;
2. `2` **at the moment** it is black — starts the trace, which runs to the
   window close, so the bad frames are in `run.log`;
3. `1` **again after** it happened — writes the bad state to `ram-2-full.bin`.

`3` arms the trace with its own edge state for a second attempt in the same run.
The two full images are the same buffers in both states, so one run compares
them. Then:

```
grep -n "BLACK DRAW" ~/Desktop/ogre-battle-64-recomp/black-sprite/run.log
```

* a hit names the draw, its rectangle, its tile and whether a palette was
  loaded — that is the faulty part, and the next step is its writer;
* no hit, with `colour:` lines present, says the black part is **not** a
  `G_TEXRECT`: it is a textured triangle or an S2DEX object, and the trace has to
  grow to cover that path (`tools/RT64/src/gbi/rt64_gbi_s2dex.cpp`'s
  `drawObjRectangle` is where an S2DEX object becomes an RDP rectangle).

## 8. What was built for this

* `OGRE_RAM_KEY_FULL=1` writes the whole 8 MiB image, and **every press** now
  writes another dump (`ram-<n>-full.bin`), so one run can compare two poses.
  `console::key_down` is new for that.
* `OGRE_TEX_TRACE_MS=0` holds the trace from the press to the window close, so
  every frame after the press is in the log.
* `OGRE_TEX_TRACE_KEY2` arms the same trace on a second key with its own edge
  state (`console::key_pressed`'s `slot`), so a press that was too early does
  not consume the next one.
* `OGRE_TEX_TRACE_COLOUR=1` / `OGRE_TEX_TRACE_BLACK=<n>` decode the sampled
  window and report a black draw by name (§6).
* `debug/black-sprite-capture.sh` sets all of them and loads a save.
* `tools/ogre_ram.py` reads a whole image: `find-ptr` (every word equal to an
  address), `cell` (one slot as the game's own CI4 image), `empty-cells`.
