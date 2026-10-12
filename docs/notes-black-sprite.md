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

Session 132 settles this: **the whole image, display lists included, is stored
in ROM order.** The file bytes at a guest address, read big-endian, are the
guest word; the file byte at `a & 0x1FFFFFFF` is the logical byte at address
`a`. The proof is the part cache: with ROM order the developer's
`ram-1-full.bin` gives `D_8018FC88 = 0x80269640`, `D_80190F70 = 0x80271640`
(+0x8000), `D_8018FC90 = 0x80271840` (+0x8200) and `D_80190F74 = 0x80271990`
(+0x8350), which are exactly the offsets `func_8017909C` computes. A byte-
reversed read of the same bytes gives `0x40962680`, `0x40162780`, `0x40182780`,
`0x90192780`.

The authority remains the renderer's own walker, `OGRE_TEX_TRACE`, which reads
the lists the way RT64's `texrect` does.

Sessions 130/131 read this image with the LE/`^ 3` rules (`tools/ogre_ram.py`,
`tools/rdram.py`), so their `find-ptr` hits and their cell renderings are
transposed inside every 4-byte group. That is what produced the "12x16 cell with
a palette at `+0x60`" reading, the "`+0x60` is a palette" line, and the wrong
`first non-zero at +72` offset. Both tools now read ROM order.

The images at `0x802FB590` and `0x802FD700` are still not flat: rendering them
as one RGBA16 image gives a dithered pattern. `func_801ACE00` writes
`D_801969D0` (the image) and `D_80197198` (a second pointer) from the scene's
object, and the draws around those addresses carry `imgw=1` with a `60x240`
window, so the address names a structure or a swizzled buffer rather than the
image's first texel.

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

## 3b. The record layout, and a withdrawn claim

**Withdrawn in the same session it was made.** §3b first said the draws bind a
record whose image is at `+8`, i.e. a `SETTIMG` eight bytes low. That is not
supported. The measurement it rested on tested for a `u16` code at
`timg - 8 + 0x78`; the cache's real link fields are at record `+0x00`/`+0x04`
(measured: 255 pointer words at offset 0 and 255 at offset 4 of the `0x80`
stride, and no pointer words at `+0x70`/`+0x74`), so the word at `timg - 8` is
the record **upstream** of the draw's record, and its `+0x78` code is a
coincidence of two adjacent records, not the draw's own.

What is verified about the record:

| offset | what |
|---|---|
| `+0x00`, `+0x04` | next and previous record on the cache's list |
| `+0x08`..`+0x67` | the 4bpp payload the loader fills, 8-byte rows |
| `+0x78` | the record's `u16` code |
| `+0x7A` | the resolved flag |

`func_8017AE40` and `func_8017AD5C` both return `record + 8` for the image
(`0x8017B0B0` `addiu v0,v0,8`; `0x8017ADBC` `addiu v0,a1,8`), the loader DMAs
the sheet row to `record + 8` (`0x8017AF5C`), and `func_8017AC28` writes the
expanded payload at `record + 8` (`0x8017AF74`). So the draw's `timg` should be
`record + 8`, and the observation that it is not is a real question — but it is
**not established** that it is wrong, because the glyph bytes the trace dumps do
render as letters (a capital `D`, a `u`, a `T` in ordered rows). An off-by-8
read would show each glyph shifted by one row and its first row blanked, which
would be visible. Until a capture shows a mis-rendered *text* glyph, treat the
`timg` offsets in the text draws as correct and this section as closed.

The verified part of the original finding stands: `D_8018FC88` is a heap base
that changes per run; the presence bitmap is at `+0x8000`, sheet A at `+0x8200`
and the code table at `+0x8350`; and a code that does not resolve makes the
consumers zero a `0x70`-byte buffer, which is a black tile by the game's own
design.

## 3c. The fault is the *replacement* head sprite (developer, session 132)

Developer, on the black part in the shot: **the character is a flat 2D sprite in
a dialogue close-up, and the head renders correctly at the start of the shot.
The fault appears when the character moves its head down and the game replaces
the head sprite.** The scene is a close-up cutscene, and the fault is per-part
and per-pose, not per-shot.

That rules out "the whole part cache was never loaded" and narrows the fault to
the state the second head sprite depends on: either the replacement is a
different code that does not resolve on this visit (a consumer then zeroes its
buffer), or it is composed into a second buffer on the frame the pose changes and
that compose does not run.

## 3d. What the developer's two pose dumps show (session 132)

The two whole-image dumps the developer took either side of the pose change
(`black-sprite/ram-1-full.bin`, `-ram-2-full.bin`) differ by **263124 bytes
(3.14%)**, which `tools/rdram.py diff` groups as 2878 runs in heap/asset space.
Session 131's "9 changed bytes in `0x80260000..0x80300000`" is therefore wrong:
the two images are not near-identical.

The scene's bound images, compared byte-for-byte between the poses, split in two.
Most hold identical content. Eight lose most of it:

| bound image | pose A non-zero | pose B non-zero |
|---|---|---|
| `0x8026A4C8` | 235 | 159 |
| `0x8026A548` | 277 | 135 |
| `0x8026A5C8` | 361 | 141 |
| `0x8026A648` | 464 | 240 |
| `0x8026A6C8` | 418 | 274 |
| `0x8026A748` | 446 | 368 |
| `0x802FB0A0` | 460 | 306 |
| `0x802FB590` | 305 | 299 |

**Two readings of this that are withdrawn in the same session.**

1. Three records (`0x8026A640`, `0x8026A6C0`, `0x8026A740`) carry code `0x1FFF`
   in pose A while their payloads hold 97..121 non-zero bytes, and real codes
   (`0x0051`, `0x0089`, `0x007B`) in pose B. The first reading called that "the
   free list and the payloads disagree, so the cache is corrupt". It is not:
   `0x1FFF` is what `func_8017909C` writes into **every** record at
   initialisation (`0x80179184`), and that routine `bzero`s only the presence
   bitmap, so a record the allocator has never handed out keeps whatever the
   block held. A `0x1FFF` code with a populated payload is an unused record with
   stale bytes — the normal state of the free list, not a fault.
2. The "eight bound images lose most of their content" table was read as churn
   under pressure. It is not. Counting the code fields of the whole table:

   | pose | records in use | distinct codes | free (`0x1FFF`) |
   |---|---|---|---|
   | A | 32 | 32 | 224 |
   | B | 35 | 35 | 221 |

   Only **3 records change code** between the poses (slots 32, 33, 34, to
   `0x0051`, `0x0089`, `0x007B`), and the cache is **35 of 256 records full**.
   There is no eviction pressure, so the LRU cannot be recycling a record a
   draw still points at. What changes between the poses is which records the
   scene has asked for, not the cache running out.

**One negative test on the mechanism.** The leading candidate for "exactly one
part black" is the code stream's sheet switch (`docs/guides/sprite-mechanisms.md`
§2): `func_8017A740` returns `-1` for any code the loaded sheet does not carry,
and the stream's `0x8140` command switches which sheet is loaded. In **both** of
the developer's pose dumps the working sheet `D_80190F60` is `0x027203D2` —
`func_8009DD28(0x218C17E)`, the base sheet — and both selectors
`D_8018FC8C`/`D_8018FC98` are 0. The switch did not fire between those two
presses. That is not a refutation in general, because both presses were taken
with the head black, so the pair brackets the fault and not the good pose; it
does mean the sheet is not what differs between those two frames.

`func_8009DD28(a0) = a0 + 0x594254`, so reading `D_80190F60` from a dump names
the sheet directly: `0x027203D2` = `0x218C17E`, `0x02721DCA` = `0x218DB76`,
`0x0270216E` = `0x216DF1A`. That value in the developer's dump matches
`func_8009DD28(0x218C17E)` exactly, which also confirms the dump is the scene
whose cache is loaded.

## 4. What is open

1. **Which routine writes a cache record during the pose change, and whether it
   runs before or after the frame's display list is built.** §3d measured the
   state (three records whose code is `0x1FFF` while their payload still holds
   97..121 stale bytes, refilled with real codes on the other side).
   `tools/watch.sh 0x8026A648 --after <symbol>` names the writer. This is the
   step that decides the fix.
2. **Whether the replacement head sprite the developer sees comes from this
   cache at all.** The measurement in §3d is on the text glyphs. The close-up
   character art is drawn as 40x48 and 258x61 RGBA16 rectangles and no capture
   has it on screen with the fault, so the connection is a hypothesis, not a
   result. A capture with the fault present and a full-screen screenshot (not
   the zoom) is what settles it; `OGRE_CAPTURE_PRESENT` yields black PPMs on
   this machine, so the screenshot has to come from the desktop.
3. **The difference between the two routes.** Both are playable and the mission
   enter is the same, so the difference is cache and heap state left by the
   route. §3d shows the cache is a live 256-record working set whose free list
   and payloads move; a route that leaves it in a different state can leave one
   part unresolved for one pose.

## 4b. Withdrawn

- Session 131's "the 11x14 rectangles are the font cache's glyph cells, an
  all-zero cell decodes to the transparent colour 0, and the two captures are
  near-identical (9 changed bytes)": the byte order was reversed and the two
  images differ by 263124 bytes. §2, §3d.
- Session 132's own "the draws bind `record` instead of `record + 8`": the test
  read the upstream record's code field. §3b.

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

## 9. Session 133: the fault is captured, and it is not a `G_TEXRECT`

The developer drove to the fault on `scene_14.bin` and pressed `1` five times;
press 1 has the head correct and presses 2-5 have it black. See
`docs/HANDOFF-2026-10-11-session133.md` for the full evidence.

**The frame is in the dump.** The game's own framebuffers
(`D_800A8204 = {0x80000400, 0x80025C00, 0x8004B400}`, 320x240 RGBA5551, `0x25800`
apart) hold the rendered frame, 3D terrain and units included; the three agree to
2-9 bytes of 153600 sampled. So a key press captures the fault for offline
viewing, with no screenshot and no `OGRE_CAPTURE_PRESENT`.
`tools/ogre_frame.py` renders a dump's `G_TEXRECT` layer into a PNG.

**What the fault looks like.** The head is a **flat `0x2947`** (RGB555 `5,5,3` =
8-bit `41,41,25`) block over the correct head silhouette, in all three
framebuffers, in every failing press. `0x2947` is the frame's own
`G_SETENVCOLOR` `0x2A291F >> 3`, so the head is drawn in the ENV colour with the
texture's alpha and no texture RGB.

**What the unit is.** 14 quads (2 textured triangles each), each with two
textures: an I4 mask sampled from an I8 source at TMEM 408 `tmem=408`, and an
RGBA16 colour at TMEM 0. The head is the 16x16/16x15 square part:

| | correct press | failing presses |
|---|---|---|
| colour | `0x802C6BE8` | `0x802FB0E8` |
| mask | `0x802C6DF0` | `0x802FB2D0` |
| block | preloaded run `0x802C5280`..`0x802C9EB0` | runtime block `0x802FB0E0` |

**The runtime block is built by `func_8019ABC4`** (`0x8019ABC4`): it takes a
source asset, sizes from its `u16` at `+4`/`+6`, `malloc(s7+fp+0x10)`, writes
`36 34 00 02 w h` at the block and `36 34 04 00 w h` at `+8+s7`, then fills two
payloads (the `& 4` source flag selects a copy + a palette expansion; the clear
flag copies both payloads raw). Its callers are `func_801AD558` and
`func_801AD6C4`; `func_8019A1B4`, `func_80199EEC` and `func_8019A010` are the
cache/decode accessors.

**At dump time the block's bytes are correct**: the RGBA16 payload decodes to the
head-down art and the stride really is 16 texels (row-to-row difference over
strides 14/15/16/17 is `13387/12438/6259/10477`). The draw's state is identical in
the two presses. So the rendered colour and the dumped bytes disagree, and §6 of
the handoff names the one experiment (`OGRE_TEX_TRACE_IMAGES=1`) that says whether
the renderer read the buffer before the game wrote it.

**Second driven run (same session).** With `OGRE_TEX_TRACE_IMAGES=1` the
fingerprint at display-list submission reports the head's payloads populated
(`0x802FB0E8`: `nonZero 710/1024, mean 91.1`; `0x802FB2D0`: `nonZero 179/256,
mean 125.0`), so the renderer did read the right bytes and the fault is in its
sampling. An RSP-style walk of the frame shows the unit is drawn in two passes;
the second uses combiner `FC62FE03 2F0D73FD` = `(1 - TEXEL1) * ENV + TEXEL1` with
`alpha = PRIM_A = 0xFF` (opaque), so `TEXEL1 = 0` renders exactly ENV, and the
head's box is 71 pixels of `0x2947 = ENV >> 3` against 176/145 distinct values on
the trunk and legs. The head's tile descriptors match every part that renders, so
the question is now why RT64 samples this one draw's `TEXEL1` as zero.

**Withdrawn here:** session 131/132's reading of the unit as a `G_TEXRECT` path.
The trace's `0 'BLACK DRAW'` lines were inevitable: the unit is triangles, and the
trace's colour decode also uses the `G_SETTIMG` width as the row stride and the
last `G_SETTIMG` as the image, which for the dialogue portrait is the palette.

## 10. Session 134: the fault is RT64's framebuffer tile copy, and it is fixed

Session 133's open question was why the head's `TEXEL1` samples as zero while its
RDRAM, its tile descriptors and its combiner are all right. It samples as zero
because **that tile never becomes a texture**: RT64 answers it from a framebuffer
tile copy whose contents were never written for those texels. See
`docs/HANDOFF-2026-10-11-session134.md` for the runs and the log lines.

**The path.** `FramebufferManager::checkRegionsTMEM` accepts a tile copy when a
region that covers the tile's TMEM range has a valid `fbTile` and the line width
and pixel size match (or can be reinterpreted). The head's colour tile
(`tmem 0..60`, `lineWidth 16`, `siz 2`, `fmt 0`) matched a region over a
framebuffer at masked `0x2E6840` whose `lastWriteFmt` is `G_IM_FMT_DEPTH`
(`[fbcopy] … fbFmt=5`), so the tile was served from that framebuffer's GPU copy
instead of `TextureManager::uploadTexture`, and therefore has no `[dec]` line.
Thirteen of the unit's fourteen colour tiles do have one.

**Why the copy is empty.** `FramebufferManager::createTileCopyRecord`
(`rt64_framebuffer_manager.cpp:183`) copies exactly the framebuffer's
`lastWriteRect` out of the depth target (then discards the rect), so a tile outside
that rect samples texels the copy never had. The head's tile is
`468,65..484,67`, and the framebuffer's declared range
(`addressStart + imageRowBytes(width) * height`, the depth image's address with the
colour image's width and `framebufferHeightForDisplay`'s height) reaches into the
same heap the game builds the head-down bitmap in. The framebuffer never rendered
those texels; the game wrote them with the CPU.

**The fix** refuses a copy whose texels are not inside the framebuffer's
`lastWriteRect`, so the load reads RDRAM the way hardware does. A blanket refusal
of depth framebuffers as colour sources also fixed the head, but it changed one
scenery scanline: a one-row strip legitimately samples the same framebuffer's
rendered rows, which the depth-to-RGBA16 copy converts. The write-rect test keeps
that strip and refuses only the head. Verified from the dump at the fault moment:
the head's box has 1 pixel of the flat `0x2947` (was 76) and the correct sprite art,
and `y=120` carries its pre-fix values; developer-confirmed.

**Two consequences for the next reader.** `FixedRect::fullyInside` reads backwards
for its name: `A.fullyInside(B)` is true when B is inside A. And a freshness test
against `fb->RAMHash` is unreliable on its own, because `hashTracking` re-hashes
every framebuffer at the end of every frame without touching the GPU copies; that
is what made the first version of the fix render the head correctly on some frames
and not others.

## 11. Session 136: the same fault in a second cutscene, and the general guard

The developer found a second cutscene with a black part (a town street, a knight
whose head is black, a dialogue balloon *"Help! Someone, help me!!"*), reachable
from `pre_ogre.bin`, and reported that the head **flashes**: the correct sprite
lasts a single frame and the black one lasts seconds.

**The shipped guard does not fire there.** `debug/head-trace.sh` on that save
(`black-sprite-136`) gives 23 `[fbcopy]` lines — a tile served by a framebuffer
copy — and **0 `[fbcopy-outside]`**, i.e. session 134's write-rect test passes on
every one of them. All 23 name the same framebuffer: `fb=0x32AF40`,
`bytes=153600` (320x240 RGBA16), at 16-32 wide strips of one to three rows. One
`[fbcopy-stale]` line is the freshness guard firing on one frame, which is the
correct frame the developer sees.

**The fault frame's signature is session 134's.** The two `1` presses are the
fault frame: the game's own framebuffers hold the black-headed knight, and the
head's box (`140..170 x 95..135`) is 162 of 900 pixels of exactly `0x0001`, which
is the frame's only `G_SETENVCOLOR`, `0x000001`. The head is drawn in the ENV
colour because `TEXEL1` sampled as zero — the same combiner path as `0x2947` in
§10, with a different scene's ENV.

**Why the write-rect test cannot see it.** That framebuffer is not part of the
frame at all. The fault frame's display list has one `SETCIMG`, at `0x802DE890`,
and it is `addr=0x00000400`; nothing targets `0x32AF40`. The record is a leftover
from an earlier scene, and its `lastWriteRect` is the stale `0,0..319,239` that
covers every one of those tiles. What sits in that range at fault time is the
game's CPU-built unit sprite scratch: 7 of the frame's 264 texture loads read from
inside `0x32AF40..0x350740`, the unit's blocks span `0x329098..0x32C770`, the head
payloads are at `0x32A318`, `0x32A5C0` and `0x32B0B0`, and the blocks carry
`36 34 00 02 w h` headers at `0x32C1F0`, `0x32CC40`, `0x32D850`, `0x32DF80`,
`0x32E350`. The region's words are 78.9% `0xFFFE` and 11.3% `0xFFFC`, a leftover
image rather than sprite art.

**The general guard.** A tile copy holds the framebuffer's GPU content, and that
is the truth only for the workload that rendered it; after that the content is
written back to RDRAM and the game may reuse the addresses. `checkRegionsTMEM` now
also requires the source framebuffer to have been written **in the workload being
processed** (`Framebuffer::lastWriteWorkload` against the manager's per-workload
`usedTimestamp`, stamped in `State::submitFramebufferPair`), and prints
`[fbcopy-nowrite]` for that refusal. The per-load site is required: a `RegionTMEM`
stamped in an earlier workload survives in `activeRegionsTMEM`, and
`checkRegionsTMEM` re-reads the framebuffer for every load.

**Verified.** With the guard (`black-sprite-139`) the same route gives 14
`[fbcopy-nowrite]`, all `fb=0x32AF40`, `writeRect=0,0..319,239`,
`fbWorkload=9342` against `workload=9583..9660`, and **zero `[fbcopy]`**; the
developer confirms the head renders. `OGRE_FB_GPU_COPY=0` on the same route gives
the same correct picture, which is the A/B. The scene-14 regression run
(`black-sprite-141`) still refuses the head tile there
(`[fbcopy-outside] fb=0x2E6840 fmt=5 tile=468,65..484,67 writeRect=null`), refuses
no legitimate copy (`[fbcopy-nowrite]` = 0), and its dumped frame matches the
session-134 verified dump: the unit crop's colour histogram is identical and
`y=120`, `x=110..119` is `5a49 624b 51c7 5a09 5a09 628b 6a8b 628b 72cd 7b0f` in
the new dump, the session-134 dump and the pre-fix fault dump.

**Two instrument limits this run hit.** `OGRE_SPRITE_TRACE` stops at 40000 lines
and `OGRE_TILE_TRACE_ALL` at `OGRE_TILE_TRACE_MAX=5000`; both were consumed by
call 201 of a long run, so late scenes have no `[spr]`/`[gpu-tile]` line.
`[fbcopy]`, `[fbcopy-stale]`, `[fbcopy-outside]`/`[fbcopy-nowrite]` and `[dec]`
are deduplicated instead and still print.

