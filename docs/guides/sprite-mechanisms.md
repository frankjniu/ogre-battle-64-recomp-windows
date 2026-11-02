# The sprite and text mechanisms

The game draws every 2D image as a textured rectangle (`G_TEXRECT`) that samples
a buffer in RDRAM. There is no sprite hardware and no atlas object: the address
decides what is drawn. This guide names the three producers of those rectangles,
the buffers they read, and how to check a draw. It is the reference for "a
rectangle draws black", "a glyph is missing" and "a sprite part is in the wrong
place".

## 1. What a draw is

An F3DEX2 display list sets this state and then draws:

| command | what it sets |
|---|---|
| `G_SETTIMG` (0xFD) | `timg`, the RDRAM address of the image, plus `fmt`, `siz` and `width` |
| `G_SETTILE` (0xF5) | tile 0's `line` (the source row stride in 64-bit words) and `palette` |
| `G_LOADTLUT` (0xF0) | for an indexed image, the palette |
| `G_SETTILESIZE` (0xF2) | the tile's window in texels |
| `G_TEXRECT` (0xE4) / `G_TEXRECTFLIP` (0xE5) | the on-screen rectangle in quarter pixels, the source `s`/`t` and `dsdx`/`dtdy` |

A `G_TEXRECT` that samples an all-zero region paints palette colour 0 or black.
A black sprite is a correct rectangle bound to a buffer the game never filled.

`OGRE_TEX_TRACE=1` prints one line per `G_TEXRECT` with every field above:

```
[tex] dl=2047 n=4 TEXRECT rect=84,24..95,38 px=11x14 s=0 t=0 dsdx=1024 dtdy=1024
      window=uls0 ult0 lrs60 lrt52 tile=line1 pal0 timg=0x80269CC8 fmt=2 siz=2 imgw=1 texels=12x15
```

`OGRE_TEX_TRACE_COLOUR=1` decodes the window through the palette, adds
`colour: mean N/255 ...`, and prints a `BLACK DRAW:` line per black draw.
`OGRE_TEX_TRACE_KEY=<1..9>` arms the trace on a key press and
`OGRE_TEX_TRACE_MS` closes it. `tools/ogre_tex_uv.py` reads the trace.

Read captured images through `rdram.load_image` (`tools/rdram.py`,
`tools/ogre_ram.py` do): it converts either dump layout to ROM order, where the
guest word at `a` is the four bytes at `a & 0x1FFFFFFF` read big-endian (AGENTS
rule 6).

## 2. The part cache: glyphs and unit parts

The cache holds the small images the game reuses: text glyphs, unit parts and UI
icons.

| item | address |
|---|---|
| block base | `D_8018FC88`, a heap pointer that changes per run; 0 before the cache loads |
| records | `base + 0x0000`, 256 records of `0x80` bytes |
| presence bitmap | `base + 0x8000`, `0x200` bytes: bit `code & 7` of byte `code >> 3` |
| sheet A (decompressed) | `base + 0x8200`, from ROM asset `0x218C0D4` |
| sheet B (decompressed) | `base + 0x8200 + align8(sizeA)`, from ROM asset `0x218DACC` |
| code table | `base + 0x8350 + align8(sizeB)` (`D_80190F74`), `0x1000` ascending `u16` codes; index `i` is slot `i` |
| working sheet | `D_80190F60`, the ROM address of a third sheet (not decompressed) |

The loader `func_8017909C` sizes the block (`0x14200` for the live sheets),
zeroes the presence bitmap (`0x801791D4`), decompresses sheets A and B, and links
the records into a doubly-linked list. `func_80179080` calls it with
`a0 = 0x100`; `func_801792DC` frees the block and `func_80179374` flushes it.

A record:

| offset | what |
|---|---|
| `+0x00` | previous record (`0x80179178`) |
| `+0x04` | next record (`0x80179180`) |
| `+0x08`..`+0x77` | the image: 4bpp, 8 bytes a row |
| `+0x78` | the `u16` code; the loader writes `0x1FFF` into every record (`0x80179188`), so an unused record with a populated payload is normal |
| `+0x7A` | resolved flag (0 while loading) |

The image is `record + 8`: `func_8017AE40` returns `node + 8` (`0x8017B0B0`),
`func_8017AD5C` returns `a1 + 8` (`0x8017ADBC`), and the loader DMAs and expands
the part to `node + 8` (`0x8017AF5C`, `0x8017AF74`). The cache uses 32-35 of its
256 records in the captures measured, so eviction is rare.

The accessors, in call order:

| function | what |
|---|---|
| `func_8017A740` | takes a `u16*` cursor into a byte stream (below) and returns `index + 0x200`, or `-1` |
| `func_8017A908` | `code >= 0x1000` returns 0; otherwise a set presence bit calls `func_8017AD5C`, a clear bit `func_8017AE40`. Both return the image pointer or 0 |
| `func_8017AE40` | unlinks the head record, sets its code, DMAs the part out of the working sheet (`D_80190F60 + code*0x138` for codes `< 0x100` or `>= 0x200`, `D_80190F60 + (code-0x100)*0x10` for `0x100..0x1FF`; `0x8017AF38`..`0x8017AF98`), expands it to 4bpp at `record + 8`, sets the resolved flag, and returns `record + 8` |

`func_8017AE40` reads 40 bytes of sheet row for most codes and 16 for
`0x100..0x1FF` (`0x8017AF58`, `0x8017AF88`); the expander `func_8017AC28` turns
them into `0x70` or `0x28` bytes of 4bpp.

### The code stream and the sheet switch

`func_8017A740` reads the stream at the cursor (`0x8017A750`..`0x8017A828`):

- The leading byte goes to `func_8017B570`, which returns 1 for
  `0x81 <= byte <= 0xFC`. Any other byte jumps to the sheet tail at `0x8017A83C`.
- In range, it reads two bytes big-endian. `0x8140` also goes to the sheet tail
  (`0x8017A7BC`). Any other value is binary-searched in `D_80190F74` and returns
  `index + 0x200`, or `-1` when absent.
- The sheet tail checks `D_8018FC8C`: when 0 it sets it to 1 and loads
  `func_8009DD28(0x216DF1A)` into `D_80190F60`. It then chooses the working sheet,
  `0x218DB76` when `D_8018FC98` is set and `0x218C17E` otherwise, stores it in
  `D_80190F60`, clears `D_8018FC8C` (`0x8017A858`), and returns `-1`.

`func_8009DD28(a0)` is `a0 + 0x594254`, so `D_80190F60` names the working sheet
directly: `0x027203D2` (`0x218C17E`), `0x02721DCA` (`0x218DB76`), `0x0270216E`
(`0x216DF1A`). `func_8017909C` clears the selector at cache load (`0x801790B4`)
and `func_8017AA38` saves and restores it around a message parse (`0x8017AA6C`,
`0x8017AAF4`), so the working sheet belongs to whichever script ran last.

A part is unresolved, and its consumer zeroes its buffer, when the cursor is one
byte off (a code turns into `0x8140` or into a code the sheet does not carry) or
when the loaded sheet lacks the code.

### The text compositor

`func_801A8124` is the text compositor for every scene. For each code it calls
`func_8017A740` and `func_8017A908`; a non-null image is copied to a `0x70`-byte
scratch with `func_80093060` (`0x801A81EC`), and a null image zeroes the scratch
with `func_80093380` (`0x801A8200`). Every byte of a zeroed 4bpp image is palette
index 0, so one missing code is one black tile, from the game's own code. Its
`+0x03` field selects the blit's palette or copy path (`0x801A803C`), and `+0x04`
is the per-glyph advance in texels (`0x801A8044`).

`func_801A7FA4(slot, x, y, dest)` is the 11x14 glyph blitter (`0x801A80D8`,
`0x801A80E8`). For each texel it reads the nibble at `slot + 8 + row*8 + col/2`
(high half for an even column, low half for an odd one) and, when it is non-zero,
writes the nibble minus `0x80`, masked to 8 bits, at `dest + row*8 + col/2`
(`0x801A8040`). A zero nibble leaves the destination alone.

## 3. The per-scene draw object

Many scenes keep a heap object with a preloaded table of parts and a list of draw
entries, and draw from it once a frame. Scene `0x14` (the Witch's Den) is the
example. Its enter `func_801AC944` builds it:

| step | instruction |
|---|---|
| state object | `func_80070F30(0xF4)` then bzero, stored to `D_801C94D8` (`0x801ACA1C`, `0x801ACA24`) |
| part table | `func_80070F30(0x1730)`, stored at object `+0xAC` (`0x801ACA68`, `0x801ACA74`) |
| font cache | `jal func_80179080` (`0x801ACA34`) |
| texture pointer | `func_8009DD38(0x1DD33DE)` stored at object `+0xD0` (`0x801ACA48`, `0x801ACA5C`) |

The preload loop `0x801ACA78`..`0x801ACAF8` resolves each part of an ASCII code
stream with `func_8017A740` and `func_8017A908` into `table + i*0x70`; a
non-null image is copied in (`0x801ACABC`), and a null one zeroes the entry
(`0x801ACAE8`), which later draws as a black rectangle.

The per-frame hook `func_801ACE00` sets the display-list cursor `D_800E9BA0`, the
object `D_801C94D8`, the object's `+0x08` (texture) into `D_801969D0` and its
`+0x10` into `D_80197198`, then calls `func_801AB998`. That walks up to 20
12-byte entries at `object + 0x18 + 0x0C*i` and dispatches on byte `+2`: 1 is a
string (`func_801AB568`, one glyph per character through `func_801AB27C`), 2 the
number blit (`func_801AAD2C`), anything else the plain part blit
(`func_801AB770`). Each emits its own `SETTIMG`/`SETTILE`/`SETTILESIZE`/`TEXRECT`
block.

The same shape elsewhere:

| scene | enter | object | per-frame draw |
|---|---|---|---|
| `0x14` Witch's Den | `func_801AC944` | `D_801C94D8` | `func_801ACE00` → `func_801AB998` |
| `0x12` Load Game | `func_8017808C` | `D_801C94D8` | `func_801AB998` (update `func_80178430`) |
| `0x0D` cutscene | `func_80178568` | the cutscene vtable `&D_801E5AC0` | the dialogue printer |

## 4. `sprite_rect_emit`: the map's sprite rectangles

The map (`0x05`) and the form scene (`0x07`) use a simpler producer,
`func_ovlM_8019F83C` (unit M):

- `a0 = x`, `a1 = y`, `a2 = sprite-table index`;
- a table entry is 8 bytes at `0x801A6FD8 + (a2 & 0xFFFF)*8`: `u16 W`, `u16 H`,
  `s16 u`, `s16 v` (`lui`/`addiu` at `0x8019F870`/`0x8019F874`);
- it emits `G_RDPPIPESYNC`, the `G_TEXRECT` (word 0 the lower-right corner
  `(x+W, y+H)`, word 1 the upper-left `(x, y)`, quarter pixels), `G_RDPHALF_1`
  with `s,t` (`u<<21 | v<<5`), `G_RDPHALF_2` with `dsdx = dtdy = 1.0`, and
  `G_RDPPIPESYNC`. It emits no `G_SETTILESIZE`; the caller supplies the window;
- it clips at x = 0 and y = 0 like `gSPScisTextureRectangle`. With WIDESCREEN on,
  the rectangle is rewritten with signed coordinates before `jr ra`
  (`widescreen.md`).

`func_ovlM_801A2A7C` (the party draw) makes two calls, each after its own
`SETTIMG`/`SETTILE`/`LOADBLOCK`/`SETTILESIZE` block. Call 1 (ROM `0x81B50`,
`a2 = 0xB`) is the shadow; call 2 (ROM `0x81BFC`, `a2 = 0xA`) is the party marker,
sampling `state[+0x34] + ((3*state[0x1DC] + f) << 12)`, a 24-frame sheet of
`0x1000` bytes composited at run time. `state = *(u32*)0x80197B18`, a heap
pointer. A table entry's index is not its on-screen size: check the entry, not
the call site.

## 5. Checking a draw

1. **Find the rectangle.** `OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_COLOUR=1`, with
   `OGRE_TEX_TRACE_KEY=<1..9>` to arm on a key press and `OGRE_TEX_TRACE_MS=0` to
   hold until the window closes. The `[tex]` line names the rectangle and the
   image address.
2. **Read the image.** `tools/ogre_ram.py <image> cell <addr>` prints one record
   as the game's 4bpp image with `nonZero`, `mean` and the first non-zero offset.
   `--slot 0x100` is the 0x100-byte cell a cutscene draw binds; the default
   `0x80` is the cache record. `find-ptr <addr>` lists every word equal to an
   address.
3. **Decide which side is wrong.** Zero bytes: the writer never filled the
   buffer. Correct bytes with a black mean: the palette is wrong. Non-zero bytes
   in the wrong place: the pointer is wrong.
4. **Name the writer.** `tools/watch.sh <guest-address> --after <symbol>` sets an
   lldb write watchpoint on `rdram_base + (guest - 0x80000000)` and prints a
   backtrace per hit. `--after` is the last guest function that runs before the
   window of interest; arming from boot on a heap address gives thousands of hits.
5. **Compare two moments.** `OGRE_RAM_KEY=1 OGRE_RAM_KEY_FULL=1` writes the whole
   8 MiB image on every key press, so one run brackets a change. Compare the
   images with `tools/rdram.py diff A B`.

## 6. A black part with a filled buffer

When the image bytes are correct in RDRAM but the part still draws black or as
flat colour, check the renderer's framebuffer tile copy. The game builds some
sprite parts (the knight's head in the town cutscene, unit heads in dialogue
close-ups) in CPU scratch that RT64 also holds a framebuffer record for, and a
tile load served from that stale record draws the part as flat colour. The RT64
patch serves a tile copy only when the source framebuffer was written in the
current workload and the tile lies inside its `lastWriteRect`
(`rt64_framebuffer_manager.cpp`), and otherwise reads RDRAM
(`docs/DECISIONS.md` → "Renderer"). A new black part with good bytes is a lead
on that guard.
