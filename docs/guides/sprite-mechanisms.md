# The sprite and text mechanisms

The game draws every 2D image as a textured rectangle (`G_TEXRECT`) in a display
list, and every one of them samples a buffer that lives in RDRAM. There is no
sprite hardware, no atlas object and no scene-graph node. The address alone
decides what is drawn.

This guide names the three producers of those rectangles, the buffers they read,
and the way to check one. It is the reference for "a rectangle draws black",
"a glyph is missing", "a sprite part is in the wrong place" and the questions in
the session 131-133 black-sprite notes (git history).

## 1. What a draw is

An F3DEX2 display list sets three pieces of state and then draws:

| command | what it fixes |
|---|---|
| `G_SETTIMG` (0xFD) | `timg`, the RDRAM address of the image, plus `fmt`, `siz` and `width` |
| `G_SETTILE` (0xF5) | tile 0's `line` (the source row stride, in 64-bit words) and `palette` bit |
| `G_LOADTLUT` (0xF0) | for an indexed image, the palette: it loads from `timg + 2*count` for `(count+1)*2` bytes |
| `G_SETTILESIZE` (0xF2) | the tile's declared window in texels |
| `G_TEXRECT` (0xE4) / `G_TEXRECTFLIP` (0xE5) | the on-screen rectangle in quarter pixels and the source `s`/`t` and `dsdx`/`dtdy` |

A `G_TEXRECT` that samples an all-zero region of RDRAM paints the palette's
colour 0 or black. That is the entire black-sprite failure mode: the rectangle is
correct, the address is not.

`OGRE_TEX_TRACE=1` prints one line per `G_TEXRECT` with every field above, and
the tile's `line` and palette:

```
[tex] dl=2047 n=4 TEXRECT rect=84,24..95,38 px=11x14 s=0 t=0 dsdx=1024 dtdy=1024
      window=uls0 ult0 lrs60 lrt52 tile=line1 pal0 timg=0x80269CC8 fmt=2 siz=2 imgw=1 texels=12x15
```

`rect` is the on-screen rectangle, `timg` the image address, `texels` the
sampled window (width in texels by rows). `OGRE_TEX_TRACE_COLOUR=1` decodes the
window through the palette and adds `colour: mean N/255 ...`, and prints one
`BLACK DRAW:` line per black draw. See `docs/guides/app-build.md` for the full
knob set and `tools/ogre_tex_uv.py` for reading a dump.

Two byte-order facts apply when reading a captured image offline:

```
file byte at    a & 0x1FFFFFFF   is the logical byte the game stores at guest a
struct.unpack_from('<I', image, a & 0x1FFFFFFF)[0]  is the byte-reversed word
```

The storage is in ROM order. A word the game holds as `0x80269640` appears in
the file as the bytes `80 26 96 40`, so read it big-endian (or equivalently,
read it with a tool whose documented convention you have checked against a
known pointer). The byte-reversed value `0x40962680` is what an LE read
returns; a display list and an image are both stored in ROM order and must be
read the same way. `tools/rdram.py`'s `word()` is an LE read and returns the
reversed value, while its `byte()` (`image[off ^ 3]`) returns the reversed byte;
do not mix the two conventions inside one decode.

## 2. The part cache: the source of every glyph and unit part

This cache is why a character is "composed of multiple parts". It holds the
small images the game reuses: text glyphs, unit parts and UI icons.

| item | address |
|---|---|
| block base | `D_8018FC88` (`0x8018FC88`), a heap pointer that changes per run |
| slot table | `base + 0x0000`, 256 slots of `0x80` bytes |
| presence bitmap | `base + 0x8000`, `0x200` bytes: bit `code & 7` of byte `code >> 3` |
| sheet A (decompressed) | `base + 0x8200`, from ROM asset `0x218C0D4` |
| sheet B (decompressed) | `base + 0x8200 + align8(sizeA)`, from ROM asset `0x218DACC` |
| code table | `base + 0x8350 + align8(sizeB)`, `0x1000` `u16` codes, ascending; index `i` means slot `i` |
| working sheet | `D_80190F60`, the ROM address of a third sheet (not decompressed) |

The loader `func_8017909C` (`asm/40E80.s:17123`) sizes the block as
`0x100*0x80 + 0x200 + align8(sizeA) + 0x200 + align8(sizeB) + 0x2000 + 0x200`,
which is `0x14200` for the live sheets, zeroes the presence bitmap at
`0x801791D4`, decompresses sheets A and B into the block, and builds a
doubly-linked list through the slots. It is called with `a0 = 0x100` by
`func_80179080`; its siblings free (`func_801792DC`) and flush
(`func_80179374`) the block. A run of the title scene before the cache is
loaded has `D_8018FC88 == 0`, which is the way to tell "not loaded yet" from
"loaded empty".

A cache node holds one part. The record is `0x80` bytes and the fields sit as
the live dumps show them:

| offset | what |
|---|---|
| `+0x00`, `+0x04` | next and previous record on the cache's list. Measured: 255 pointer words at each of these offsets within the `0x80` stride, and none at `+0x70`/`+0x74`. |
| `+0x08`..`+0x67` | the 4bpp payload, `0x60` bytes: 8 bytes a row for 16 rows, 12 texels wide |
| `+0x68`..`+0x77` | unused row slots, zero |
| `+0x78` | the record's `u16` code. **`0x1FFF` is the initialisation value written into every record** at `0x80179184`, so an unused record keeps whatever bytes the block held: a `0x1FFF` code with a populated payload is normal, not a fault. The cache used 32-35 of its 256 records in the captures measured, so eviction is rare. |
| `+0x7A` | resolved flag (0 while loading) |

**The image is `node + 8`.** `func_8017AE40` returns `node + 8`
(`0x8017B0B0`: `addiu v0,v0,8`), `func_8017AD5C` returns `a1 + 8`
(`0x8017ADBC`), the loader DMAs the sheet row to `node + 8` (`0x8017AF5C`,
`a1 = s0 & ~1`), and the 4bpp expander writes at `node + 8` (`func_8017AC28` is
called with `a1 = D_80190F6C + 8` at `0x8017AF74`).

Both entry points return `record + 8` for the image, so the address a rectangle
binds should be `record + 8` and not `record`.

A session-132 attempt to prove otherwise is **withdrawn**: it tested for a code
at `timg - 8 + 0x78`, but `timg - 8` is the record *upstream* of the draw's own
record, and its `+0x78` code belongs to that neighbour. Nothing observed shows a
mis-rendered text glyph, so the text draws' addresses are taken as correct. See
the session 131-133 black-sprite notes §3b (git history).

The accessors, in the order a caller uses them:

| function | what |
|---|---|
| `func_8017A740` (`0x8017A740`) | takes a `u16*` cursor into a byte stream, reads two bytes big-endian, and binary-searches `D_80190F74` (the code table). Returns `index + 0x200`, or `-1` when the code is absent. The sentinel `0x8140` also returns `-1`. |
| `func_8017A908` (`0x8017A908`) | `code >= 0x1000` returns 0. Otherwise tests the presence bit at `D_80190F70`; a set bit calls `func_8017AD5C`, a clear bit calls `func_8017AE40`. Both return the slot's image pointer or 0. |
| `func_8017AE40` (`0x8017AE40`) | unlinks the head slot, sets its code, DMAs the part out of the working sheet (`D_80190F60 + code*0x138` for codes `< 0x100` or `>= 0x200`, `D_80190F60 + (code-0x100)*0x10` for `0x100..0x1FF`, `0x8017AF38`..`0x8017AF98`), expands it to 4bpp into the payload at `slot + 8` (`0x8017AFC4`..`0x8017B094`), sets the resolved flag, and returns `slot + 8`. A null cursor or an absent code leaves the slot on the list and returns 0. |

### The code stream, and the sheet switch

`func_8017A740` does not take a code — it takes a **cursor into a byte stream**.
The exact control flow (`0x8017A750`..`0x8017A828`):

```
lbu  a0,0(cursor)            ; the leading byte
jal  func_8017B570           ; returns 1 iff 0x81 <= a0 <= 0xFC
beqz v0, .sheettail          ; not a part command -> the tail at 0x8017A83C
```

- **When the predicate returns 0** (`a0 < 0x81` or `a0 >= 0xFD`) the function
  jumps to `0x8017A83C`, the **sheet tail**. That tail checks `D_8018FC8C`: if
  it is 0 it sets it to 1 and calls `func_8009DD28(0x216DF1A)`, storing the
  result in `D_80190F60` (`0x8017A778`..`0x8017A78C`); then it chooses the
  working sheet — `0x218DB76` when `D_8018FC98` is set, `0x218C17E` otherwise
  (`0x8017A864`, `0x8017A87C`, `0x8017A884`) — stores it in `D_80190F60`,
  clears `D_8018FC8C` (`0x8017A858`), and **returns `-1`**.
- **When the predicate returns 1**, the function reads two bytes big-endian
  (`0x8017A79C`, `0x8017A7A8`), and `beq $v1,0x8140` at `0x8017A7BC` sends
  `0x8140` to the same sheet tail. **`0x8140` is the stream's sheet switch**, and
  its return value is the tail's `-1`, so the switch yields no part.
- **Any other two-byte value** in that range is looked up by binary search over
  the `u16` table at `D_80190F74` (`0x8017A7D0`..`0x8017A828`), returning
  `index + 0x200`, or `-1` when the code is absent from the table.

So a part is unresolved, and a consumer zeroes its buffer, whenever the two-byte
read does not land on a code the loaded sheet carries:

1. **the stream is misaligned.** The command is two bytes; a cursor one byte off
   turns a code into `0x8140` or into a code the sheet does not carry;
2. **the wrong sheet is loaded.** Each sheet covers a different code set, and
   which one is working is decided by `D_8018FC8C` and `D_8018FC98`. A scene
   whose script selects a sheet that lacks the code it then asks for resolves
   that code to `-1`.

### The record's small and large parts

`func_8017AE40` reads the sheet row with `func_8009DA50` at `0x8017AF58` with
`a2 = 0x28` (40 bytes) for most codes, or `0x10` bytes for
`0x100 <= code < 0x200` (`0x8017AF88`, `0x8017AF90`). Its expander
`func_8017AC28` converts 4 bytes of source into 4 bytes of 4bpp at a time
(`0x8017AC44`..`0x8017AD44`), producing `0x70` bytes for the large form and
`0x28` for the small one. Both forms land at `record + 8`.

### The `-1` path is a black tile by construction

`func_801A8124` (`asm/1CE040.s:18292`, the text compositor) shows the consumer
side. For each code it calls `func_8017A740`, then `func_8017A908`, and:

- a non-null image is copied to the 0x70-byte scratch with `func_80093060`
  (`0x801A81EC`);
- a null image **zeroes the scratch** with `func_80093380(sp+0x128, 0x70)`
  (`0x801A8200`).

The zeroed scratch is then blitted by `func_801A7FA4`. Every byte of an all-zero
4bpp image is palette index 0, so the draw paints colour 0. One missing code is
one black tile, from the game's own code, with no port involvement.

`func_801A8124`'s other fields come from the same source: `+0x03` selects the
blit's palette-indexed or copy path (`0x801A803C`, `func_801A7FA4`), and `+0x04`
is the per-glyph advance in texels (`0x801A8044`).

### The 11x14 glyph blit

`func_801A7FA4` (`0x801A7FA4`) is the glyph blitter. It takes
`a0 = slot` (the slot, not the image), `a1 = x`, `a2 = y`, `a3 = destination`, and
walks 11 columns by 14 rows (`0x801A80D8`, `0x801A80E8`). For each column it
reads the 4bpp nibble at `slot + 8 + row*8 + col/2`, then writes it into the
destination at `dest + row*8 + col/2` when the value is non-zero:

- the nibble is the source byte's high half for an even column and its low half
  for an odd one (`0x801A8020`, `0x801A802C`);
- the destination value written is the nibble **minus 0x80** (`0x801A8040`),
  masked to 8 bits. The glyph payload therefore stores palette indices with bit
  7 set, and a zero source byte means "leave the destination alone".

The blit reads `slot + 8 + row*8 + col/2` for rows 0..13, that is
`slot + 8` .. `slot + 8 + 0x6F`, which is the whole payload.

## 3. The cutscene/UI object: a per-scene draw list

Many scenes keep a small heap object that holds a preloaded table of parts and
a list of draw entries, then draw from it once a frame. Scene `0x14` (the
Witch's Den) is the worked example, and the shape recurs.

`func_801AC944` (`0x801AC944`, scene `0x14`'s enter) builds it:

| step | instruction |
|---|---|
| state object | `func_80070F30(0xF4)` then bzero, stored to `D_801C94D8` (`0x801ACA1C`, `0x801ACA24`) |
| part table | `func_80070F30(0x1730)` = 5936 bytes, stored at object `+0xAC` (`0x801ACA68`, `0x801ACA74`) |
| font cache | `jal func_80179080` (`0x801ACA34`) |
| texture pointer | `func_8009DD38(0x1DD33DE)` stored at object **`+0xD0`** (`0x801ACA48`, `0x801ACA5C`) |
| shape | `func_80073164(0, 1, 1, 512, ...)` (`0x801AC9E0`..`0x801ACA0C`) |

The part preload loop is `0x801ACA78`..`0x801ACAF8`:

- `0x801ACAA4`: the part table entry is `table + i*0x70`, from
  `sll a1,i,3` / `subu a1,a1,i` / `sll a1,a1,4`, i.e. `i * 112`;
- `0x801ACA84`: `func_8017A740(&cursor)` — the ASCII string is the code stream;
- `0x801ACA90`: `func_8017A908(code)`;
- non-null (`0x801ACABC`): `func_80093060(image, entry, 0x70)` copies the 0x70-byte
  payload into the entry;
- null (`0x801ACAE8`): `func_80093380(entry, 0x70)` **zeroes the entry**.

So a part the cache does not resolve is a zeroed entry in the scene's own table,
and the frame's draw of that entry is a black rectangle. This is the same
failure as `func_801A8124`'s, one level up.

The per-frame draw is `func_801ACE00` (`0x801ACE00`, scene `0x14`'s hook). It
writes the display-list cursor (`D_800E9BA0`), the object it draws from
(`D_801C94D8`), the object's `+0x08` (the texture) into `D_801969D0`, and the
object's `+0x10` into `D_80197198` (`0x801ACE5C`..`0x801ACE80`), then calls
`func_801AB998` (`0x801ACF3C`). `func_801AB998` walks up to 20 entries of 12
bytes at `object + 0x18 + 0x0C*i` (`0x801AB9E0`..`0x801AB9F4`) and dispatches on
byte `+2`:

| `+2` | callee | what |
|---|---|---|
| 1 | `func_801AB568` | a string: `func_801AB27C` renders one glyph per character, `func_801AAD2C`/`func_801AB770` the two other blits |
| 2 | `func_801AAD2C` | the number/counter blit |
| other | `func_801AB770` | the plain part blit |

Each emits its own `G_SETTIMG`/`G_SETTILE`/`G_SETTILESIZE`/`G_TEXRECT` block into
the cursor. `func_801AB27C` (`0x801AB27C`) is a TMEM-window glyph renderer: it
splits the character code into a row and column (`sra t7,a3,4` /
`subu t5,v0,a2` at `0x801AB29C`..`0x801AB2A8`) and emits an `0xE4` rectangle with
the destination page as the texture source (`0x801AB374`, `0x801AB3B0`).

### The same shape elsewhere

| scene | enter | object | preload | per-frame draw |
|---|---|---|---|---|
| `0x14` Witch's Den | `func_801AC944` | `D_801C94D8` | `0x801ACA78` | `func_801ACE00` -> `func_801AB998` |
| `0x12` Load Game | `func_8017808C` | `D_801C94D8` | (`func_80178430` update) | `func_801AB998` |
| `0x0D` cutscene | `func_80178568` | the cutscene vtable `&D_801E5AC0` | — | the dialogue printer |

The compositor pair `func_801A8124` -> `func_801A7FA4` is the text path for
every scene, and scene `0x18` (Load Game) reaches it from `func_80178430`.

## 4. `sprite_rect_emit`: the map's sprite rectangles

The map scene `0x05` (and the form scene `0x07`) uses a different, simpler
producer. `func_ovlM_8019F83C` (`bankRec05`, unit M, `0x8019F83C`) is the
builder:

- `a0 = x`, `a1 = y`, `a2 = sprite-table index`;
- an entry of `g_sprite_table` is 8 bytes at `0x801A6FE0 + (a2 & 0xFFFF)*8`:
  `u16 W`, `u16 H`, `s16 u`, `s16 v` (`0x8019F87C`);
- it emits `G_RDPPIPESYNC`, the `G_TEXRECT` (word 0 is the lower-right corner
  `(x+W, y+H)`, word 1 the upper-left `(x, y)`, both in quarter pixels),
  `G_RDPHALF_1` carrying `s,t` (`u<<21 | v<<5`), `G_RDPHALF_2` with
  `dsdx = dtdy = 1.0`, and `G_RDPPIPESYNC`.
- it emits **no** `G_SETTILESIZE`. The caller supplies the window.

`func_ovlM_801A2A7C` (`0x801A2A7C`, `map_party_draw`) is the caller: two
`sprite_rect_emit` calls, each preceded by its own `SETTIMG`/`SETTILE`/
`LOADBLOCK`/`SETTILESIZE` block. Call 1 (ROM `0x81B50`, `a2=0xB`) is the
shadow; call 2 (ROM `0x81BFC`, `a2=0xA`) is the party marker, sampling
`state[+0x34] + ((3*state[0x1DC] + f) << 12)`, a 24-frame sheet of `0x1000`
bytes. `state` is `*(0x80197B18)`, a heap pointer.

Two traps live here, both from sessions 61-65:

- the table entries and the `a2` indices were read one entry late until session
  65, because unit M's `.data` was linked 8 bytes above its ROM address. A
  sprite descriptor read one entry late names a different sprite's `(W,H,u,v)`.
- the number of a table entry is not its on-screen size. Check the entry, not
  the call site.

## 5. Checking a draw

1. **Find the rectangle.** `OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_COLOUR=1`, with
   `_KEY=<1..9>` to arm on a key press and `_MS=0` to hold until the window
   closes. The `[tex]` line names the rectangle and the image address.
2. **Read the image.** `tools/ogre_ram.py <image> cell <addr>` renders one
   slot as the game's own 4bpp image with `nonZero`, `mean` and the first
   non-zero offset. `--slot 0x100` is the 0x100-byte cell a cutscene draw
   binds; the default `0x80` is the cache slot.
3. **Decide which side is wrong.** A window whose bytes are zero is a
   writer-side fault: the buffer was never filled. A window whose bytes are
   correct but whose colour mean is black is a palette-side fault. A window
   whose bytes are non-zero and whose rectangle is in the wrong place is a
   pointer-side fault.
4. **Name the writer.** `tools/watch.sh <guest-address> --after <symbol>` sets an
   lldb write watchpoint on `rdram_base + (guest - 0x80000000)` and prints a
   backtrace per hit. `--after` is the last symbol that runs before the window
   of interest; arming from boot on a heap address produces thousands of hits.
5. **Compare two runs.** `OGRE_RAM_KEY=1 OGRE_RAM_KEY_FULL=1` writes the whole
   8 MiB image on every key press, so one run brackets a change. Read the pairs
   with `tools/rdram.py diff A B`.

## 6. What is not known

There are two separate faults in this area, and they must not be conflated.

**Fault A (withdrawn).** Session 132 first reported that the text draws bind
`record` instead of `record + 8`. The measurement did not support it; the claim
is withdrawn in the session 131-133 black-sprite notes §3b (git history). What remains worth knowing is the
sheet switch itself: `func_8017909C` clears the selector at cache load
(`0x801790B4`) and `func_8017AA38` saves and restores it around a message parse
(`0x8017AA6C`, `0x8017AAF4`), so the working sheet is owned by whichever script
ran last.

**Fault B: a cutscene character's replacement head sprite draws black.** The
developer reports a flat 2D character in a dialogue close-up, correct head at the
start of the shot, and **the black one is the replacement sprite after the
character moves its head down** (session 132). It is not fault A: the 11x14
rectangles are the dialogue text.

**The leading candidate is the sheet switch above**, and it is now specific
enough to test without guessing:

- the code stream's `0x8140` command reloads the working sheet from one of
  `0x216DF1A`, `0x218DB76`, `0x218C17E`, chosen by `D_8018FC8C` and `D_8018FC98`,
  and it clears `D_8018FC8C` on the way through (`0x8017A858`);
- if the replacement head's code is in a sheet that is **not** the one working
  at that moment, `func_8017A740` returns `-1` for it, the compositor zeroes a
  `0x70`-byte buffer, and the draw paints index 0 — a black part, while the rest
  of the character (whose codes are in the loaded sheet) still draws;
- the flag is script-driven, and the two routes into the cutscene leave the
  script in different places, which is the shape of a path-dependent,
  one-part-only fault.

Two ways to test it, both cheap once a capture exists:

1. from the faulty `timg`, read the record's code at `+0x78` and compare it with
   `D_80190F74`'s table: a code absent from the table is the `-1` path, and the
   record's `+0x7A` resolved flag says whether the loader ran at all;
2. read `D_8018FC8C` and `D_8018FC98` in the two pose dumps and check whether the
   working sheet `D_80190F60` points into the same asset in both. `D_80190F60`
   is the ROM address `func_8009DD28` returned, so the three assets are
   distinguishable by value.

**Test 2 is done, and it is negative.** In both of the developer's pose dumps
(`black-sprite/ram-1-full.bin`, `-ram-2-full.bin`) the working sheet
`D_80190F60` is `0x027203D2` — `func_8009DD28(0x218C17E)`, the base sheet — and
both selectors `D_8018FC8C`/`D_8018FC98` are 0. The sheet did **not** switch
between those two presses. That does not clear the mechanism in general: both
presses were taken while the head was black, so the pair brackets the fault and
not the good pose. It does mean the switch is not what distinguishes these two
frames.

`func_8009DD28(a0) = a0 + 0x594254`, so the three working-sheet values are
`0x027203D2` (`0x218C17E`), `0x02721DCA` (`0x218DB76`) and `0x0270216E`
(`0x216DF1A`). Reading `D_80190F60` in a dump names the sheet with no lookup.

Two further unknowns:

- Which producer draws the cutscene close-up. It is neither
  `func_ovlM_801A2A7C` (the map's party sprite) nor the scene-`0x14` object; the
  cutscene module is `bankRec10a` (unit J) and the draws bind 40x48 and 258x61
  RGBA16 planes. `tools/ogre_ram.py find-ptr` on a bound address finds only the
  `G_SETTIMG` operands in the display-list buffers, so the address is computed
  from a pointer, not stored as a constant.
- Whether a black part in any given capture is a cache miss (`-1` -> zeroed
  buffer, `func_801A8124` at `0x801A81FC` and scene `0x14`'s preload at
  `0x801ACAE8`), an eviction (the LRU list is 256 records), or a stale pointer.
  A capture with the fault on screen and the code the scene asked for separates
  them.
