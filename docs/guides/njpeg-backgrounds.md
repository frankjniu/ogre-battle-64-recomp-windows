# njpeg backgrounds

The dialogue, cutscene and battle backdrops are njpeg images: Huffman-coded YUV
that the game decodes partly on the CPU and partly in an RSP microcode.
`tools/njpeg.py` decodes them offline from the ROM. This guide covers
the asset format, the pipeline, the readback rule, and how to check a backdrop.
The replacement pack that writes over these backdrops is `hd-backgrounds.md`; the
microcode is `rsp-microcode.md`.

## The pipeline

1. **Asset.** An njpeg blob in the ROM (format below).
2. **Decode.** The CPU Huffman decode `func_8008B250`, then four `M_NJPEGTASK`
   (RSP task type 4) tasks with the microcode at `0x8009ED80`, which decode to
   16-bit YUV in place.
3. **Draw.** The gfx ucode `0x800A5110` (S2DEX 2.08) draws the macroblocks into a
   game framebuffer: `0xDC` is `G_OBJ_MOVEMEM` and `0xDA` is `G_OBJ_RECTANGLE_R`.
   A YUV `LOADTILE` is the RDP's two-plane format, and the colour matrix is the
   game's `G_SETCONVERT` (AGENTS.md → "njpeg backgrounds").
4. **CPU readback.** `func_ovlE_8019976C` (bank unit E) copies the framebuffer
   out, one pass per chunk, each with its own decode and draw. The same module
   is also in record 10 and record 10b (bank C, which the battle scene `0x0E`
   loads) and in the title module; `hd-backgrounds.md` → "njpeg images by
   asset" lists the four copies and their state pointers.
5. **Blit.** The game draws the assembled backdrop with the F3DEX2 ucode
   `0x8009F540` as textured quads, one `G_LOADTILE` strip per quad, from four
   RGBA16 images: `0x80243E28` (320x240, after the `'B5'` header at
   `0x80243E10`), `0x80269660` (176x240), `0x8027E090` (320x144) and `0x802948C0`
   (176x144). Strip geometry and the on-screen canvas rects are in
   `hd-backgrounds.md`.

## Asset format and inventory

```
u32 size | 'HU' | s8 qscale | u8 0 | u16 width | u16 height | 'HUFF' | u16 macroblocks | bits
```

All fields are big-endian. Every asset satisfies
`macroblocks == (width/16) * (height/16)`; a disagreement means the layout
changed. This enumerates them from the ROM (in the WSL tree, where the ROM is):

```sh
python3 - <<'EOF'
from pathlib import Path
d = Path('assets/ogre64.z64').read_bytes()
o = d.find(b'HUFF'); n = 0
while o != -1:
    w = int.from_bytes(d[o-4:o-2], 'big'); h = int.from_bytes(d[o-2:o], 'big')
    mb = int.from_bytes(d[o+4:o+6], 'big')
    assert d[o-8:o-6] == b'HU' and mb == (w // 16) * (h // 16)
    print(f"asset rom 0x{o-12:06X}  {w}x{h}  mb={mb}")
    n += 1; o = d.find(b'HUFF', o + 1)
print(n, "njpeg assets")
EOF
```

The ROM offset printed is the asset's size word. The ROM holds 75 njpeg assets:
42 are 320x240 (the first at `0x594398`), and the other 33 come in 24 sizes from
336x256 down to 128x96. `tools/njpeg.py list <rom>` prints them with their pack
keys.

41 of the 75 are chunks of 10 `'B5'` compositions: `'B5'`, a version byte (3),
a chunk count, an `s16` anchor pair, then per chunk `s16 x, s16 y, u16 w, u16 h,
u32 size, u32 0` followed by the chunk's asset (`func_ovlE_8019A0A8` reads the
entries). Eight are four-chunk dialogue backdrops like the cathedral
(`0x7CADAC`); `0x800C74` is three 336-wide chunks stacked 655 rows tall and
`0x810848` is six chunks 422x556, which look like panning scenes. The other 34
assets are single 320x240 images: 29 outdoor terrains (grass, desert, snow,
forest, swamp, coast and fort walls; the battle backdrops by their look, not yet
confirmed in a battle), a banquet hall `0x66A54A`, a village `0x682F24`, a
market `0x6E205A`, a lit hall `0x7175A6` and a book `0x8FFEA6`.

## Offline decode (`tools/njpeg.py`)

```sh
tools/njpeg.py list   <rom>              # assets, sizes, pack keys
tools/njpeg.py decode <rom> -o <dir>     # every asset as an 8-bit PNG
tools/njpeg.py prep   <rom> -o <dir>     # each composition or single image, assembled,
                                         # plus a 4x Lanczos upscaler input and a layout json
tools/njpeg.py compare <rom> <rom offset> <reference.png> [--x X --y Y]
```

The decoder repeats the game's two halves:

- **Huffman** (`func_8008B250`, `func_8008B6E0`): per macroblock four Y blocks
  with the luminance tables (`0x800AA0A0` DC, `0x800AA120` AC) and two chroma
  blocks with the chrominance tables (`0x800AA0E0`, `0x800AA290`). The tables
  are 16 `u16` counts then the values, the JPEG Annex K tables. Bits are read
  MSB first with no byte stuffing; `(0,0)` ends a block.
- **Microcode** (`njpgdspMain`; N64 JPEG manual chapter 5): DC terms are
  differences from the previous block of the same component (the Y blocks chain
  one predictor, U and V their own); the zigzag scan runs over the transposed
  block; the quantisation table at `0x800AC070` is the JPEG luminance table,
  stored transposed, scaled 1/4 for `qscale = -2`; then an inverse DCT with no
  +128 level shift.

The draw (`func_ovlE_80199130`) sets `G_SETCONVERT` `0xEC15FD5D 0x3B78E42A`
(`k0..k5 = 175, -43, -89, 222, 114, 42`) and one combiner cycle
`(TEXEL0 - K4) * K5 + TEXEL0` (`0xFC17FE2F 0x77FCF87C`). The tool applies both,
with RT64's scaling (`2K+1`/256 for k0..k3, x/255 for K4 and K5).

The microcode's output scale was measured against the port's own decode of the
cathedral (`tools/backgrounds.py extract 01`): a least-squares fit on the
320x240 chunk gave `Y' = 1.039*Y - 4.70` and chroma x1.035. With that, the
other three chunks match the port's RGBA5551 readback exactly at 83-88 % of
texels, mean error 1.0-1.4 levels of 255. The rest is the microcode's
fixed-point IDCT. The tool writes 8-bit RGB, which is a better upscaler input
than the 5-bit readback.

## The readback

`func_ovlE_8019976C`'s row loop is
`memcpy(state[0x70], state[0x64], 2*state[0x78])` per row for `state[0x7A]` rows,
with a fixed source stride of `0x280` (640 bytes, 320 px) and a destination
stride of `2*state[0x78]`. `state[0x78]` is the width in pixels
(`lh a2,0x78(state)` at `0x80199884`, `sll a2,a2,1` in the delay slot) and
`state[0x7A]` the row count (`lh v1,0x7A(state)` at `0x801998B4`);
`state = *(u32*)0x8019A680`. The four passes and their destinations are the
table in `hd-backgrounds.md`.

The copy's source must be the game's own framebuffer choice `state[0x64]`.
RT64's scratch word `0x807FFC08` is set only by a YUV-texture-then-colour-image
pair and never cleared, so at the first pass of an assembly it still names the
previous step's target, and the backdrop then shows part of the previous screen
in its top-left chunk. `tools/njpeg_readback.py` (re-applied by
`make bank-recomp`) makes the copy read `state[0x64]` and call
`ogre_sync_framebuffers()`, which writes the RDP's pixels back to RDRAM first.
The game's DP-completion wait already orders the copy after the render, so no
other wait is added.

## Traps

- `tools/njpeg_readback.py` (the source rule and `ogre_sync_framebuffers()`)
  patches only bank E's copy. Bank C's two copies (battle and record 10b) copy
  `state[0x64]` with no write-back first; whether a battle backdrop can show a
  stale chunk is not checked.
- The three game framebuffers are contiguous 320x240 buffers,
  `D_800A8204 = {0x80000400, 0x80025C00, 0x8004B400}`, `0x25800` apart. Reading
  `0x80000400` for more than 240 rows shows the next buffer's content. A
  mis-sourced read shows a neighbour, which looks like a timing bug.
- The framebuffer holds the four chunks in a 2x2 arrangement that is not the
  on-screen order. The on-screen position of a chunk comes from the blit that
  consumes the four buffers, not from the framebuffer render.

## Checking a backdrop

```sh
OGRE_NO_AUDIO=1 OGRE_NJREAD_LOG=1 OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=2500 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,start,a,start,a,a,a,a" \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=25000 "$APP" "$ROM" 2> njread.log
grep njread njread.log
```

One line per pass. At the New Game cathedral, `sig` cycles `573FF47B8A5A3279`,
`48892A76C362A4BA`, `3F79B6732153B2FD`, `38BCBEBE5B555C3C`, and `alt` (what the
scratch word would have read, `0` when the two agree) is `0`. A non-zero `alt` at
the first pass of an assembly is the stale-backdrop fault; its value names the
previous screen. To inspect the result, take a live-console dump triggered from
game state (`app-build.md` → "njpeg and HD backgrounds").
