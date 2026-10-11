#!/usr/bin/env python3
"""Decode the OGRE_TEX_TRACE dumps: what each textured rectangle sampled.

`app/src/renderer.cpp`'s `tex_trace` pass walks the game's submitted display
list, prints one `[tex]` line per `G_TEXRECT` with the rectangle, the texel
window and the image state (SETTIMG address/format/size/width, tile line,
palette, SETTILESIZE), and, with `OGRE_TEX_TRACE_DUMP=<dir>`, writes the
sampled RDRAM bytes to `<dir>/tex/NNNNNN_src.bin` (and `_pal.bin` for an indexed
image) beside a `.json` describing them.

This tool turns that into something readable:

    tools/ogre_tex_uv.py /tmp/texrun            # per-draw summary
    tools/ogre_tex_uv.py /tmp/texrun --png      # + a PNG of each image
    tools/ogre_tex_uv.py /tmp/texrun --dark     # only draws whose sample is black

The point is the black-part question: a draw whose source decodes to black is a
different fault from a draw whose source is fine and whose window reads outside
the intended image, and the summary separates the two.

Input is the run's log (the `[tex]` lines) plus the dump directory. Pass the log
path explicitly when it is not `<dir>/log.txt`:

    tools/ogre_tex_uv.py /tmp/texrun --log /tmp/texrun/run.log
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import struct
import sys

LINE = re.compile(r"^\[tex\] dl=(\d+) n=(\d+) (\w+) rect=(-?\d+),(-?\d+)\.\.(-?\d+),(-?\d+) "
                  r"px=(\d+)x(\d+) s=(-?\d+) t=(-?\d+) dsdx=(-?\d+) dtdy=(-?\d+) "
                  r"window=uls(\d+) ult(\d+) lrs(\d+) lrt(\d+) tile=line(\d+) "
                  r"pal(\d+) timg=0x([0-9A-Fa-f]+) fmt=(\d+) siz=(\d+) imgw=(\d+) texels=(\d+)x(\d+)")


def bits_per_texel(siz: int) -> int:
    return 4 << siz


def bytes_per_row(siz: int, width: int) -> int:
    return (width * bits_per_texel(siz)) // 8


def decode_rgba16(word: int):
    r = (word >> 11) & 0x1F
    g = (word >> 6) & 0x1F
    b = (word >> 1) & 0x1F
    a = word & 1
    return (r * 255 // 31, g * 255 // 31, b * 255 // 31, 255 if a else 0)


def decode_ia16(word: int):
    i = (word >> 8) & 0xFF
    a = word & 0xFF
    return (i, i, i, a)


def decode_ia8(byte: int):
    i = (byte >> 4) & 0xF
    a = byte & 0xF
    v = i * 17
    return (v, v, v, a * 17)


def decode_ia4(nib: int):
    i = (nib >> 1) & 0x7
    a = nib & 0x1
    v = i * 255 // 7
    return (v, v, v, 255 if a else 0)


def texel_at(data: bytes, siz: int, width: int, s: int, t: int, palette=None):
    """The colour of the texel at (s,t) of a linear image, or None."""
    bpr = bytes_per_row(siz, width)
    row = t * bpr
    if row < 0 or row >= len(data):
        return None
    if siz == 0:                      # 4b
        off = row + (s >> 1)
        if off >= len(data):
            return None
        nib = (data[off] >> 4) if (s & 1) == 0 else (data[off] & 0xF)
        if palette:
            v = palette[nib]
            return decode_rgba16(v[0] | (v[1] << 8)) if v is not None else None
        return decode_ia4(nib)
    if siz == 1:                      # 8b
        off = row + s
        if off >= len(data):
            return None
        b = data[off]
        if palette:
            v = palette[b]
            return decode_rgba16(v[0] | (v[1] << 8)) if v is not None else None
        return (b, b, b, 255)
    if siz == 2:                      # 16b
        off = row + s * 2
        if off + 1 >= len(data):
            return None
        return decode_rgba16(data[off] | (data[off + 1] << 8))
    off = row + s * 4             # 32b
    if off + 3 >= len(data):
        return None
    return (data[off + 3], data[off + 2], data[off + 1], data[off])


def load_palette(path: str, count=None):
    if not os.path.exists(path):
        return None
    data = open(path, "rb").read()
    out = []
    for i in range(0, len(data) - 1, 2):
        out.append((data[i], data[i + 1]))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", help="the OGRE_TEX_TRACE_DUMP directory (holds tex/)")
    ap.add_argument("--log", default=None, help="the run's log (default <dir>/log.txt)")
    ap.add_argument("--png", action="store_true", help="write a PNG per dumped image")
    ap.add_argument("--dark", action="store_true", help="print only draws whose sample is black")
    ap.add_argument("--top", type=int, default=0,
                    help="print only the N darkest samples, sorted by mean luminance")
    ap.add_argument("--limit", type=int, default=0, help="stop after N reported draws")
    args = ap.parse_args()

    tex_dir = os.path.join(args.dir, "tex")
    if not os.path.isdir(tex_dir):
        print("%s has no tex/ subdirectory; run with OGRE_TEX_TRACE_DUMP=%s"
              % (args.dir, args.dir), file=sys.stderr)
        return 2
    log_path = args.log or os.path.join(args.dir, "log.txt")
    if not os.path.exists(log_path):
        print("no log at %s; pass --log" % log_path, file=sys.stderr)
        return 2

    dumped = {}
    for j in glob.glob(os.path.join(tex_dir, "*.json")):
        try:
            meta = json.load(open(j))
        except (ValueError, OSError):
            continue
        dumped[meta["draw"]] = meta

    ranked = []
    printed = 0
    for line in open(log_path, "r", errors="replace"):
        m = LINE.match(line)
        if not m:
            continue
        (dl, n, op, x0, y0, x1, y1, pxw, pxh, s, t, dsdx, dtdy, uls, ult, lrs, lrt,
         line_v, pal, timg, fmt, siz, imgw, texw, texh) = m.groups()
        n = int(n)
        meta = dumped.get(n)
        desc = ("dl=%s n=%s %s rect=%s,%s..%s,%s px=%sx%s s=%s t=%s dsdx=%s dtdy=%s "
                "window=%s,%s,%s,%s timg=%s fmt=%s siz=%s imgw=%s texels=%sx%s"
                % (dl, n, op, x0, y0, x1, y1, pxw, pxh, s, t, dsdx, dtdy, uls, ult, lrs, lrt,
                   timg, fmt, siz, imgw, texw, texh))

        if meta is None:
            print(desc + "  (no dump)")
            printed += 1
            if args.limit and printed >= args.limit:
                break
            continue

        src_path = os.path.join(tex_dir, "%06d_src.bin" % n)
        data = open(src_path, "rb").read() if os.path.exists(src_path) else b""
        pal_data = None
        pal_meta = dumped.get(n) if False else None
        pal_path = os.path.join(tex_dir, "%06d_pal.bin" % n)
        if os.path.exists(pal_path):
            pal_data = load_palette(pal_path)

        siz_i, imgw_i = int(siz), int(imgw)
        bpr = bytes_per_row(siz_i, imgw_i)
        # The window's own texel range, from the tile's s/t (1/16 texel in the
        # RDP's SETTILESIZE fields) and the rectangle's texel span.
        s_a, t_a = int(s) // 32, int(t) // 32
        s_b, t_b = s_a + int(texw) - 1, t_a + int(texh) - 1
        colours = []
        for tt in range(t_a, t_b + 1):
            for ss in range(s_a, s_b + 1):
                c = texel_at(data, siz_i, imgw_i, ss, tt, pal_data)
                if c is not None and c[3] > 0:
                    colours.append(c)
        if colours:
            mean = tuple(sum(c[i] for c in colours) // len(colours) for i in range(3))
            alpha = sum(c[3] for c in colours) // len(colours)
            summary = "opaque texels=%d mean=%s alpha=%d" % (len(colours), mean, alpha)
        else:
            summary = "NO OPAQUE TEXEL in the sampled window"

        black = (not colours) or (max(mean) < 24)
        lum = 0 if not colours else (mean[0] + mean[1] + mean[2]) // 3
        ranked.append((lum, len(colours), desc, summary, black))
        if args.dark and not black:
            printed += 1
            continue
        if not args.top:
            print(desc + "  " + summary + ("   <-- BLACK" if black else ""))

        if args.png:
            try:
                from PIL import Image
            except ImportError:
                print("  (--png needs PIL)", file=sys.stderr)
                args.png = False
            else:
                w = imgw_i
                h = len(data) // bpr if bpr else 0
                if w > 0 and h > 0:
                    im = Image.new("RGBA", (w, h))
                    px = im.load()
                    for yy in range(h):
                        for xx in range(w):
                            c = texel_at(data, siz_i, imgw_i, xx, yy, pal_data)
                            px[xx, yy] = c if c else (0, 0, 0, 0)
                    im = im.resize((w * 4, h * 4), Image.NEAREST)
                    im.save(os.path.join(tex_dir, "%06d_src.png" % n))

        printed += 1
        if args.limit and printed >= args.limit:
            break

    if args.top:
        ranked.sort(key=lambda r: (r[0], r[1]))
        for lum, ncol, desc, summary, black in ranked[:args.top]:
            print("%3d  %s  %s%s" % (lum, desc, summary, "   <-- BLACK" if black else ""))

    print("\n%d draw(s) reported" % printed, file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
