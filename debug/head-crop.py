"""Render the map unit's screen area out of a whole-RDRAM dump.

The black head fault is visible in the game's own framebuffer, so a key press
captures it (no screenshot). Usage:

    python3 debug/head-crop.py <ram-N-full.bin> [out.png] [x y w h] [scale]

Defaults: out.png next to the dump, the unit area 108,70..212,150, scale 4.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'black-sprite-133'))
from pngwrite import fb_to_png, half  # noqa: E402


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(path)[0] + '-unit.png'
    crop = tuple(int(v) for v in sys.argv[3:7]) if len(sys.argv) > 6 else (108, 70, 104, 80)
    scale = int(sys.argv[7]) if len(sys.argv) > 7 else 4
    with open(path, 'rb') as f:
        d = f.read()
    # The three game framebuffers hold the same frame; 0x80000400 is the one the VI
    # scans out (D_800A8204).
    fb_to_png(d, 0x80000400, out, scale, crop)
    # A flat ENV region is the signature: count the most common colours in the crop.
    hist = {}
    for y in range(crop[1], crop[1] + crop[3]):
        for x in range(crop[0], crop[0] + crop[2]):
            v = half(d, 0x80000400 + (y * 320 + x) * 2)
            hist[v] = hist.get(v, 0) + 1
    print(out)
    for v, n in sorted(hist.items(), key=lambda kv: -kv[1])[:6]:
        r = ((v >> 11) & 31) * 255 // 31
        g = ((v >> 6) & 31) * 255 // 31
        b = ((v >> 1) & 31) * 255 // 31
        print(f"  0x{v:04X} rgb({r},{g},{b}) n={n}")


if __name__ == '__main__':
    main()
