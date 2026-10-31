#!/usr/bin/env python3
"""Render the frame a display list draws, straight out of a whole-RDRAM dump.

A `OGRE_RAM_KEY=1 OGRE_RAM_KEY_FULL=1` press writes `<OGRE_PREF_DIR>/ram-<n>-full.bin`,
the whole 8 MiB guest image in ROM order. This tool walks the display lists in
that image the way the RDP does and composites every `G_TEXRECT` into a PNG, so
the frame the game actually drew can be looked at without a screenshot and
without `OGRE_CAPTURE_PRESENT` (which yields black PPMs on Metal).

What it models, all of it read from the dump and not from a guess:

* `G_SETTIMG` / `G_SETTILE` / `G_SETTILESIZE` / `G_LOADBLOCK` / `G_LOADTILE` /
  `G_LOADTLUT` state per tile;
* the RDP's TMEM indirection: a `G_TEXRECT` samples tile T at TMEM offset
  `T.tmem`, and the RDRAM image behind that offset is the one the **last load
  into it** bound, not the `G_SETTIMG` that happens to sit before the rectangle
  (the palette load re-binds `timg`, which is what makes the old in-app trace
  read the palette as the image);
* the palette: a tile's `palette` field selects the TLUT at TMEM
  `0x100 + 16*palette`, loaded by `G_LOADTLUT`;
* the row stride: the tile's `line` in 64-bit words, which is the sampling
  stride, and not the `G_SETTIMG` width (that one is for `G_LOADBLOCK`);
* CI4, CI8, RGBA16, RGBA32, IA4/IA8/IA16 and I4/I8 decode, alpha = the
  palette/texel low bit.

Usage:
    tools/ogre_frame.py <dump> --log <run.log> --out <dir>   # one PNG per DL root in the log
    tools/ogre_frame.py <dump> --dl 0x802B5E00 --out <file.png>
    tools/ogre_frame.py <dump> --dl 0x802B5E00 --draws      # print the draws, no image
    tools/ogre_frame.py <dump> --scan                       # every PLausible DL root

`--log` reads the `[renderer] display list N ... data=0x...` lines the app
prints with `OGRE_DL_TRACE=1`, which name the root display list of a frame. The scene's display lists
are double buffered, so the log's last few roots are the ones to try, and the
tool renders them all so the frame nearest the press can be picked by eye.

The byte order is the dump's: the file byte at `a & 0x1FFFFFFF` is the logical
byte the game holds at guest `a`, and a guest word is those four bytes
big-endian. See docs/guides/sprite-mechanisms.md.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rdram import load_image  # noqa: E402

RDRAM_SIZE = 0x800000
W, H = 320, 240

# GBI opcodes this walker understands (F3DEX2).
OP_SETTIMG = 0xFD
OP_SETTILE = 0xF5
OP_SETTILESIZE = 0xF2
OP_LOADBLOCK = 0xF3
OP_LOADTILE = 0xF4
OP_LOADTLUT = 0xF0
OP_TEXRECT = 0xE4
OP_TEXRECTFLIP = 0xE5
OP_DL = 0xDE
OP_ENDDL = 0xDF
OP_MOVEWORD = 0xDB
OP_BRANCH_Z = 0x04
OP_SETOTHERMODE_H = 0xE3
OP_RDPSETOTHERMODE = 0xEF


class Image:
    def __init__(self, data: bytes):
        self.d = data

    def word(self, a: int) -> int:
        o = a & 0x1FFFFFFF
        return int.from_bytes(self.d[o:o + 4], "big")

    def half(self, a: int) -> int:
        o = a & 0x1FFFFFFE
        return int.from_bytes(self.d[o:o + 2], "big")

    def byte(self, a: int) -> int:
        return self.d[a & 0x1FFFFFFF]

    def ok(self, a: int, n: int) -> bool:
        return 0 <= (a & 0x1FFFFFFF) and (a & 0x1FFFFFFF) + n <= RDRAM_SIZE


def p0(v: int, pos: int, bits: int) -> int:
    return (v >> pos) & ((1 << bits) - 1)


class Tile:
    __slots__ = ("fmt", "siz", "line", "tmem", "palette", "uls", "ult", "lrs", "lrt")

    def __init__(self) -> None:
        self.fmt = self.siz = self.line = self.tmem = self.palette = 0
        self.uls = self.ult = self.lrs = self.lrt = 0


class Draw:
    __slots__ = ("at", "tile", "fmt", "siz", "line", "palette", "ulx", "uly", "lrx", "lry",
                 "s", "t", "dsdx", "dtdy", "src", "pal", "flip", "uls", "ult", "lrs", "lrt",
                 "q", "othermode_h")

    def key(self):
        return (self.ulx, self.uly, self.lrx, self.lry, self.src, self.pal, self.s, self.t)


def walk(img: Image, start: int, limit: int = 20000, depth: int = 0, mode: list | None = None):
    """Decode the display list at `start`; return the draws it emits.

    `mode` is a one-element list holding the RDP's othermode high word, shared
    with the lists this one calls so a mode set in a callee stays set."""
    if mode is None:
        mode = [0]
    tiles = [Tile() for _ in range(8)]
    tmem_src: dict[int, int] = {}
    tlut_src: dict[int, int] = {}
    timg = timg_fmt = timg_siz = 0
    timg_w = 1
    draws: list[Draw] = []
    a = start
    n = 0
    while n < limit:
        if not img.ok(a, 8):
            break
        w0 = img.word(a)
        w1 = img.word(a + 4)
        op = w0 >> 24
        n += 1
        if op == OP_SETTIMG:
            timg, timg_fmt, timg_siz, timg_w = w1, p0(w0, 21, 3), p0(w0, 19, 2), p0(w0, 0, 12) + 1
            a += 8
        elif op == OP_SETTILE:
            t = tiles[p0(w1, 24, 3)]
            t.fmt, t.siz, t.line, t.tmem = p0(w0, 21, 3), p0(w0, 19, 2), p0(w0, 9, 9), p0(w0, 0, 9)
            t.palette = p0(w1, 20, 4)
            a += 8
        elif op == OP_SETTILESIZE:
            t = tiles[p0(w1, 24, 3)]
            t.uls, t.ult = p0(w0, 12, 12), p0(w0, 0, 12)
            t.lrs, t.lrt = p0(w1, 12, 12), p0(w1, 0, 12)
            a += 8
        elif op in (OP_LOADBLOCK, OP_LOADTILE):
            t = tiles[p0(w1, 24, 3)]
            t.uls, t.ult = p0(w0, 12, 12), p0(w0, 0, 12)
            t.lrs, t.lrt = p0(w1, 12, 12), p0(w1, 0, 12)
            tmem_src[t.tmem] = timg
            a += 8
        elif op == OP_LOADTLUT:
            t = tiles[p0(w1, 24, 3)]
            src = timg + ((p0(w0, 12, 12) >> 2) << timg_siz >> 1)
            tlut_src[t.tmem] = src
            a += 8
        elif op in (OP_TEXRECT, OP_TEXRECTFLIP):
            tile = p0(w1, 24, 3)
            t = tiles[tile]
            st, ds = img.word(a + 12), img.word(a + 20)
            def sext(v, bits):
                return v - (1 << bits) if v >= (1 << (bits - 1)) else v
            d = Draw()
            d.at = a
            d.tile = tile
            d.fmt, d.siz, d.line, d.palette = t.fmt, t.siz, t.line, t.palette
            d.uls, d.ult, d.lrs, d.lrt = t.uls, t.ult, t.lrs, t.lrt
            d.ulx, d.uly = p0(w1, 12, 12) // 4, p0(w1, 0, 12) // 4
            d.lrx, d.lry = p0(w0, 12, 12) // 4, p0(w0, 0, 12) // 4
            d.q = (p0(w1, 12, 12), p0(w1, 0, 12), p0(w0, 12, 12), p0(w0, 0, 12))
            d.othermode_h = mode[0]
            d.s, d.t = sext((st >> 16) & 0xFFFF, 16), sext(st & 0xFFFF, 16)
            d.dsdx, d.dtdy = sext((ds >> 16) & 0xFFFF, 16), sext(ds & 0xFFFF, 16)
            d.flip = (op == OP_TEXRECTFLIP)
            d.src = tmem_src.get(t.tmem, timg)
            want = 0x100 + t.palette * 16
            cands = [k for k in tlut_src if k <= want]
            d.pal = tlut_src[max(cands)] if cands else None
            draws.append(d)
            a += 24
        elif op == OP_SETOTHERMODE_H:
            # F3DEX2: w0 bits 8..15 = 32 - shift - len, bits 0..7 = len - 1.
            length = p0(w0, 0, 8) + 1
            shift = 32 - p0(w0, 8, 8) - length
            if 0 <= shift < 32:
                mask = ((1 << length) - 1) << shift
                mode[0] = (mode[0] & ~mask & 0xFFFFFFFF) | (w1 & mask)
            a += 8
        elif op == OP_RDPSETOTHERMODE:
            mode[0] = w0 & 0x00FFFFFF
            a += 8
        elif op == OP_DL:
            if depth < 8:
                draws.extend(walk(img, w1, limit, depth + 1, mode))
            a += 8
        elif op == OP_ENDDL:
            break
        else:
            a += 16 if op == OP_BRANCH_Z else 8
    return draws


def texel(img: Image, d: Draw, ts: int, tt: int):
    """(r, g, b, a) for texel (ts, tt) of the draw's source image, or None."""
    if d.src is None:
        return None
    bpr = (d.line * 8) if d.line else None
    if bpr is None:
        # No tile line: fall back to the image width the loader used.
        return None
    base = d.src + tt * bpr
    fmt, siz = d.fmt, d.siz
    if fmt == 2:                                  # CI
        if siz == 0:
            b = img.byte(base + (ts >> 1))
            idx = (b >> 4) if (ts & 1) == 0 else (b & 0xF)
            idx += d.palette * 16
        elif siz == 1:
            idx = img.byte(base + ts) + d.palette * 16
        else:
            return None
        if d.pal is None:
            return None
        c = img.half(d.pal + idx * 2)
        return ((c >> 11) & 31) * 255 // 31, ((c >> 6) & 31) * 255 // 31, ((c >> 1) & 31) * 255 // 31, c & 1
    if fmt == 0:                                  # RGBA
        if siz == 2:
            c = img.half(base + ts * 2)
            return ((c >> 11) & 31) * 255 // 31, ((c >> 6) & 31) * 255 // 31, ((c >> 1) & 31) * 255 // 31, c & 1
        if siz == 3:
            o = base + ts * 4
            return img.byte(o), img.byte(o + 1), img.byte(o + 2), img.byte(o + 3)
        if siz == 1:                              # IA8
            c = img.byte(base + ts)
            i, al = (c >> 4) * 17, (c & 0xF) * 17
            return i, i, i, 1 if al else 0
    if fmt == 3:                                  # IA
        if siz == 0:                              # IA4
            b = img.byte(base + (ts >> 1))
            v = (b >> 4) if (ts & 1) == 0 else (b & 0xF)
            i, al = (v & 0xE) * 17, (v & 1) * 255
            return i, i, i, 1 if al else 0
        if siz == 1:
            c = img.byte(base + ts)
            i, al = (c >> 4) * 17, (c & 0xF) * 17
            return i, i, i, 1 if al else 0
        if siz == 2:
            c = img.half(base + ts * 2)
            i, al = (c >> 8) & 0xFF, c & 0xFF
            return i, i, i, 1 if al else 0
    if fmt == 4:                                  # I
        if siz == 0:
            b = img.byte(base + (ts >> 1))
            v = ((b >> 4) if (ts & 1) == 0 else (b & 0xF)) * 17
            return v, v, v, 1
        if siz == 1:
            v = img.byte(base + ts)
            return v, v, v, 1
    return None


def composite(img: Image, draws, bg=(16, 16, 28), scale=1):
    buf = bytearray(bytes(bg) * (W * scale) * (H * scale))
    painted = 0
    for d in draws:
        # RT64 skips a rectangle whose tile covers no texels; so does hardware.
        if d.line == 0 or d.lrs == d.uls or d.lrt == d.ult:
            continue
        x0, x1 = max(0, d.ulx), min(W, d.lrx)
        y0, y1 = max(0, d.uly), min(H, d.lry)
        if x1 <= x0 or y1 <= y0:
            continue
        for y in range(y0, y1):
            for x in range(x0, x1):
                du = x - d.ulx
                if d.flip:
                    du = (d.lrx - d.ulx) - du
                ts = (d.s + d.dsdx * du // 32) // 32
                tt = (d.t + d.dtdy * (y - d.uly) // 32) // 32
                if ts < 0:
                    ts = 0
                if tt < 0:
                    tt = 0
                px = texel(img, d, ts, tt)
                if px is None or px[3] == 0:
                    continue
                for sy in range(scale):
                    for sx in range(scale):
                        o = (((y * scale + sy) * W * scale) + (x * scale + sx)) * 3
                        buf[o] = px[0]
                        buf[o + 1] = px[1]
                        buf[o + 2] = px[2]
                painted += 1
    return buf, painted


def write_png(path: Path, w: int, h: int, rgb: bytes) -> None:
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9))
                     + chunk(b"IEND", b""))


def log_roots(log: Path, limit: int = 6):
    """The `[renderer] display list N ... data=0x...` roots, newest last."""
    pat = re.compile(r"\[renderer\] display list (\d+) .*data=(0x[0-9A-Fa-f]+)")
    roots = []
    for line in log.read_text(errors="replace").splitlines():
        m = pat.search(line)
        if m:
            roots.append((int(m.group(1)), int(m.group(2), 16)))
    return roots[-limit:]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", type=Path)
    ap.add_argument("--dl", type=lambda s: int(s, 0), help="root display list address")
    ap.add_argument("--log", type=Path, help="run.log to take root display lists from")
    ap.add_argument("--out", type=Path, help="PNG path, or a directory with --log")
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--draws", action="store_true", help="print each draw instead of writing an image")
    ap.add_argument("--scan", action="store_true", help="try every SETTIMG-adjacent address as a root")
    args = ap.parse_args()

    img = Image(load_image(args.dump))
    if len(img.d) != RDRAM_SIZE:
        print(f"{args.dump}: {len(img.d)} bytes, expected {RDRAM_SIZE}", file=sys.stderr)
        return 2

    roots: list[tuple[str, int]] = []
    if args.dl is not None:
        roots.append((f"dl{args.dl:08X}", args.dl))
    if args.log is not None:
        for n, a in log_roots(args.log):
            roots.append((f"log{n}_{a:08X}", a))
    if args.scan:
        for o in range(0, RDRAM_SIZE - 8, 8):
            if img.d[o] == OP_SETTIMG:
                roots.append((f"seed{0x80000000 + o:08X}", 0x80000000 + o))
    if not roots:
        print("no display list address: pass --dl, --log or --scan", file=sys.stderr)
        return 2

    status = 0
    for name, addr in roots:
        draws = walk(img, addr)
        uniq = []
        seen = set()
        for d in draws:
            if d.key() in seen:
                continue
            seen.add(d.key())
            uniq.append(d)
        if args.draws:
            print(f"== {name} ({len(uniq)} draws)")
            for d in uniq:
                # `q` is the rectangle in pixels with the quarter-pixel fraction
                # kept; `cyc` is the RDP cycle type (0 1-cycle, 1 2-cycle,
                # 2 copy, 3 fill) and `filt` the texture filter (0 point,
                # 2 bilinear, 3 average), both from the othermode high word.
                q = ",".join(f"{v / 4:g}" for v in d.q[:2]) + ".." + ",".join(f"{v / 4:g}" for v in d.q[2:])
                print(f"  {d.at:08X} rect={d.ulx},{d.uly}..{d.lrx},{d.lry} tile={d.tile} "
                      f"fmt={d.fmt} siz={d.siz} line={d.line} pal={d.palette} "
                      f"s={d.s} t={d.t} dsdx={d.dsdx} dtdy={d.dtdy} "
                      f"src={('0x%08X' % d.src) if d.src else '-'} "
                      f"tlut={('0x%08X' % d.pal) if d.pal else '-'} "
                      f"q={q} cyc={p0(d.othermode_h, 20, 2)} filt={p0(d.othermode_h, 12, 2)} "
                      f"win={d.uls / 4:g},{d.ult / 4:g}..{d.lrs / 4:g},{d.lrt / 4:g}")
            continue
        buf, painted = composite(img, uniq, scale=args.scale)
        if args.out is None:
            print(f"{name}: {len(uniq)} draws, {painted} texels painted")
            continue
        out = args.out
        if out.suffix.lower() != ".png":
            out = out / f"frame-{name}.png"
            out.parent.mkdir(parents=True, exist_ok=True)
        write_png(out, W * args.scale, H * args.scale, buf)
        print(f"{name}: {len(uniq)} draws, {painted} texels painted -> {out}")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
