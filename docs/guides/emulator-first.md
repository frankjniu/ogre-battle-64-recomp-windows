# Emulator-first debugging

Read this before interpreting a display list or a buffer the port produced.

An emulator does not interpret a display list. It runs the game's code and then
a faithful RDP over whatever that code wrote. A display list the port emitted is
evidence about the port. It becomes evidence about the game only after the
port's code, its ucode dispatch and its RDRAM are shown to be faithful at that
point (AGENTS.md rule 8).

## 1. What the port has, compared with an emulator

| piece | emulator | this port |
|---|---|---|
| GBI identification | hash the task's ucode, pick the matching GBI | RT64's `GBIManager::getGBIForUCode` does the same on every `send_dl` (§3) |
| RSP | HLE known ucodes, LLE the rest | the njpeg and audio microcodes run recompiled; any other task is stubbed and produces no output (`docs/guides/rsp-microcode.md`) |
| ground truth | it is the reference | none built in; borrow a reference emulator (§5) |

## 2. What does not count as evidence

- **A decoded display list.** `OGRE_DL_DECODE` and `OGRE_DL_ANALYZE` decode every
  list as F3DEX2. They print plausible geometry for an S2DEX2 list. Hash the
  ucode instead (§3).
- **A constant read out of the ROM.** It is the game's own data. Patching the
  generated C to change it fabricates game data (AGENTS.md rule 7) and hides the
  mechanism that differs.
- **An exit RDRAM dump alone.** `OGRE_DUMP_RDRAM` writes at exit. It says what a
  buffer holds after everything ran, not what it held at the draw. Use the live
  console or `OGRE_RAM_KEY` for "at the moment X happens".
- **A capture from a probe build.** A frame counts only from a tree regenerated
  with `make bank-recomp && make recomp` (AGENTS.md rule 6).

## 3. Which GBI did this display list get?

RT64 hashes the ucode with XXH3-64 over the host RDRAM bytes at the task's
`ucode` and `ucode_data` addresses, at the segment lengths in its database
(`tools/RT64/src/gbi/rt64_gbi.cpp`). `OGRE_DL_TRACE=1` prints the task's ucode
on every submission:

```
[renderer] display list 853 at t=8278ms entries=0 (type=1 ucode=0x8009F540 data=0x8028C120)
```

Text segments are hashed at `0x1390` (F3DEX2) or `0x18C0` (S2DEX2); data
segments at `0x420` or `0x390`. `tools/rdram.Dump.data` is in ROM order, and RT64
hashes host order, so byte-swap each 32-bit word before hashing:

```python
import xxhash                          # pip install xxhash
from tools.rdram import Dump
d = Dump('C:/tmp/map.bin')
off, n = 0x8009F540 & 0x1FFFFFFF, 0x1390
rom = d.data[off:off + n]
host = b''.join(rom[i:i + 4][::-1] for i in range(0, n, 4))
print(hex(xxhash.xxh3_64(host).intdigest()))
```

Known results:

| ucode | length | hash | RT64 GBI |
|---|---|---|---|
| `0x8009F540` (main gfx) | `0x1390` | `0xCF55FAE288BFE48D` | `F3DEX2.fifo 2.08` |
| `0x800A5110` (njpeg draw) | `0x18C0` | `0x9300F34F3B438634` | `S2DEX2.fifo 2.08` |

A ucode that matches nothing makes RT64 print `Unable to find a matching GBI in
the current database`. If that line is absent from a run log, every list was
dispatched by hash.

## 4. The five checks

Run these before forming a theory about what the game meant. Together they place
a difference in one of: recompiler, ucode dispatch, renderer, RDRAM contents, or
game state.

1. **Which GBI?** §3.
2. **Did an RSP task go missing?** Run with `OGRE_RSP_LOG=1`, then
   `tools/runlog.py <run.log>`. It lists tasks by type and flags any non-gfx task
   the stub swallowed.
3. **Is the code the code we compiled?** `tools/rdram.py <dump> banks` names the
   module resident at each streamed RAM window. `make midfunc` lists the
   prologue-less tails.
4. **Are the bytes what hardware would have?** Decode the asset with an
   independent implementation. `tools/ogrelz.py` is one for the game's LZ
   format; its stream must end exactly on the asset's declared payload boundary.
5. **Is the recompiler faithful at this call?** N64Recomp emits a `jal` as the
   delay-slot instruction, then the call. The copy of the delay slot after
   `goto after_N` is dead code for a fall-through entry; the delay slot runs once.
   Compare the generated C at the `jal` (commented with both guest addresses),
   `mips-linux-gnu-objdump -d` of the owning ELF, and the ROM bytes at
   `bank_rom_start + (ram - bank_ram_start)`.

If all five pass, the port does what hardware would do for this state. The
difference is then in the state the game reached (forced scene against the
natural route, save pre-state, timing). Compare with a reference emulator's RDRAM
at the same screen before changing anything. A path that works only with state
the game never sets is a finding to write down (AGENTS.md rule 7).

## 5. Borrowing a reference emulator

RetroArch is installed at `C:\RetroArch-Win64` with the
`mupen64plus_next_libretro.dll` core; parallel-n64 also works.

1. **Reference RDRAM.** Play the ROM to the screen, save a state, and extract
   RDRAM with mupen64plus-core's `savestate.c` (vendored under
   `tools/RT64/src/contrib/mupen64plus-core/src/device/`). Diff it against the
   port's image at the same guest addresses.
2. **Screenshot A/B.** The same screen in the reference emulator next to a port
   capture (`OGRE_CAPTURE_PRESENT`). Retail references from the developer are in
   `docs/proofs/intro-movie-reference/` and `docs/proofs/map-reference/`.
3. **mupen64plus-rsp-hle** (`src/hle.c`, `task_detection`) knows one
   OB64-specific microcode, the JPEG decoder (`jpeg_decode_OB`), which this port
   runs recompiled. Graphics HLE lives in the video plugin.

## 6. GLideN64's OB64 settings

GLideN64 carries two workarounds for this ROM. Both are in its S2DEX handler, so
they concern the njpeg backdrop path and S2DEX sprites, not F3DEX2 `G_TEXRECT`
draws.

- `ini/GLideN64.custom.ini`, section `[OGREBATTLE64]`:
  `graphics2D\enableTexCoordBounds=1`. It clamps texture-rectangle coordinates
  to their native-resolution values (`S2DEX_Obj_Sprite`). RT64 has no equivalent
  setting; the port sets RT64's `upscale2D = Original` instead
  (`docs/DECISIONS.md` → App, settings and UI).
- `src/RSP.cpp` sets `hack_Ogre64`, used only by `gSPObjRectangleR` →
  `_drawYUVImageToFrameBuffer` to copy the YUV texture to the framebuffer. The
  port's equivalent is the njpeg readback (`docs/guides/njpeg-backgrounds.md`).
