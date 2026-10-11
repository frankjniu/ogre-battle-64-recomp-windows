#!/usr/bin/env python3
"""Read a full RDRAM image (`OGRE_RAM_KEY_FULL=1`) for a black-sprite question.

Session 130 left the black-sprite bug at "a buffer that should hold one sprite
part is all zero". The window dump proves the bytes are zero but cannot say
where the rest of that part's data is, or which structure points at the empty
block. A full 8 MiB image can:

    tools/ogre_ram.py /tmp/ram-1-full.bin find-ptr 0x8026A5C8
    tools/ogre_ram.py /tmp/ram-1-full.bin cell 0x8026A5C8
    tools/ogre_ram.py /tmp/ram-1-full.bin empty-cells 0x80268000 0x8026C000

The runtime stores every 32-bit word byte-reversed, so with `d` the image:

    guest word at address a  = struct.unpack_from('<I', d, a & 0x1FFFFFFF)
    logical byte at address a = d[(a & 0x1FFFFFFF) ^ 3]

`tools/rdram.py` implements the same rules for the same files; this tool adds
the two questions a full image answers and a window cannot.

`find-ptr` reports every word in the image equal to the address, because the
display list that draws a sprite part holds the part's image address as
`G_SETTIMG`'s operand and the part table holds it as a pointer. Either hit names
the structure that chose the address.

`cell` prints a sprite part slot as the game's own format when its size is
known. The default 0x100-byte slot is the CI4 image plus palette pair the
scene-`0x0D` cutscene binds: `G_SETTIMG` fmt 2 at slot+0x00 and fmt 0 at
slot+0x80 (session 131). Use `--slot` for another layout.

Output is text; `--png` writes a picture of the slot for a visual check.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


class Image:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        if len(self.data) != 0x800000:
            print(f"warning: {path} is {len(self.data)} bytes, expected 0x800000", file=sys.stderr)

    def word(self, addr: int) -> int:
        return struct.unpack_from("<I", self.data, addr & 0x1FFFFFFF)[0]

    def half(self, addr: int) -> int:
        return struct.unpack_from("<H", self.data, addr & 0x1FFFFFFE)[0]

    def byte(self, addr: int) -> int:
        return self.data[(addr & 0x1FFFFFFF) ^ 3]

    def region(self, addr: int, length: int) -> bytes:
        off = addr & 0x1FFFFFFF
        return bytes(self.data[(off + i) ^ 3] for i in range(length))


def in_rdram(addr: int, length: int = 1) -> bool:
    return 0x80000000 <= addr <= 0x80800000 - length


def cmd_find_ptr(img: Image, addr: int, limit: int) -> int:
    print(f"words equal to 0x{addr:08X} in {img.path}:")
    found = 0
    for off in range(0, len(img.data) - 4, 4):
        if struct.unpack_from("<I", img.data, off)[0] == addr:
            guest = 0x80000000 + off
            around = [img.word(guest + 4 * k) for k in range(-2, 3)]
            print(f"  0x{guest:08X}  nearby: " + " ".join(f"{v:08X}" for v in around))
            found += 1
            if found >= limit:
                print(f"  (limit {limit} reached)")
                break
    if found == 0:
        print("  none. The address is computed, not stored; `cell` on the bound "
              "address and the caller's table are then the way in.")
    return 0


def render_slot(img: Image, addr: int, slot: int, image_len: int, ci4: bool) -> None:
    """Print the slot's image as text, one character per texel."""
    data = img.region(addr, slot)
    width = 16
    row_bytes = width // 2 if ci4 else width * 2
    rows = (image_len + row_bytes - 1) // row_bytes
    print(f"slot 0x{addr:08X}..0x{addr + slot:08X} ({slot} bytes), "
          f"image {image_len} bytes, {rows} rows of {width} texels")
    for r in range(rows):
        row = data[r * row_bytes:(r + 1) * row_bytes]
        if not row:
            break
        text = ""
        for i in range(width):
            if ci4:
                b = row[i // 2]
                v = (b >> 4) & 0xF if i % 2 == 0 else b & 0xF
                text += "." if v == 0 else format(v, "x")
            else:
                if 2 * i + 1 >= len(row):
                    text += "."
                    continue
                w = row[2 * i] | (row[2 * i + 1] << 8)
                text += "." if w == 0 else "#"
        nz = sum(1 for b in row if b)
        print(f"  r{r:2d} {text}   nz {nz:2d}/{len(row)}")


def cmd_cell(img: Image, addr: int, slot: int, png: str | None) -> int:
    head = img.region(addr, slot)
    nonzero = sum(1 for b in head if b)
    first = next((i for i, b in enumerate(head) if b), None)
    print(f"0x{addr:08X} slot {slot} bytes: nonZero {nonzero}/{slot}, "
          f"mean {sum(head) / slot:.1f}, first non-zero at "
          f"{'none' if first is None else hex(first)}")
    if nonzero == 0:
        print("  ALL ZERO: a draw that binds this address samples palette index 0 "
              "everywhere, which renders as solid black")
    # The cutscene's part is a CI4 image followed by its 16-entry palette.
    render_slot(img, addr, slot, min(slot // 2, 0x100), ci4=True)
    print("  --- second half (the palette when fmt 0 is bound here) ---")
    render_slot(img, addr + slot // 2, slot // 2, slot // 2, ci4=True)
    if png:
        write_png(img, addr, slot, Path(png))
    return 0


def write_png(img: Image, addr: int, slot: int, out: Path) -> None:
    """Write the slot's first half as a 16x16 RGBA picture (CI4, no palette)."""
    import zlib

    width = 16
    rows = 16
    raw = bytearray()
    for y in range(rows):
        raw.append(0)
        for x in range(width):
            b = img.byte(addr + y * 8 + x // 2)
            v = (b >> 4) & 0xF if x % 2 == 0 else b & 0xF
            raw += bytes((v * 17, v * 17, v * 17, 255))
    def chunk(tag: bytes, data: bytes) -> bytes:
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, rows, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    out.write_bytes(png)
    print(f"  wrote {out}")


def cmd_empty_cells(img: Image, lo: int, hi: int, slot: int, limit: int) -> int:
    if hi <= lo:
        print("empty-cells needs lo < hi", file=sys.stderr)
        return 2
    print(f"slots of 0x{slot:X} from 0x{lo:08X} to 0x{hi:08X}:")
    empties = 0
    for addr in range(lo, hi - slot + 1, slot):
        data = img.region(addr, slot)
        nz = sum(1 for b in data if b)
        if nz == 0:
            print(f"  0x{addr:08X}  ALL ZERO")
            empties += 1
            if empties >= limit:
                print(f"  (limit {limit} reached)")
                break
    print(f"  {empties} empty slot(s)")
    return 0


def parse_addr(text: str) -> int:
    return int(text, 16) if not text.startswith("0x") else int(text, 0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("find-ptr", help="every word equal to an address")
    p.add_argument("addr")
    p.add_argument("--limit", type=int, default=16)

    p = sub.add_parser("cell", help="print one sprite-part slot")
    p.add_argument("addr")
    p.add_argument("--slot", type=lambda v: int(v, 0), default=0x100)
    p.add_argument("--png")

    p = sub.add_parser("empty-cells", help="slots that are all zero in a range")
    p.add_argument("lo")
    p.add_argument("hi")
    p.add_argument("--slot", type=lambda v: int(v, 0), default=0x100)
    p.add_argument("--limit", type=int, default=32)

    args = ap.parse_args()
    img = Image(args.image)
    if args.cmd == "find-ptr":
        return cmd_find_ptr(img, parse_addr(args.addr), args.limit)
    if args.cmd == "cell":
        return cmd_cell(img, parse_addr(args.addr), args.slot, args.png)
    if args.cmd == "empty-cells":
        return cmd_empty_cells(img, parse_addr(args.lo), parse_addr(args.hi), args.slot, args.limit)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
