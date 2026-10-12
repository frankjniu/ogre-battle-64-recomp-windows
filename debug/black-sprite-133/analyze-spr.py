"""Read a debug/head-trace.sh run.log and report what the renderer bound.

The unit's pass 2 is the 2-cycle pass (`combine=FC62FE03/2F0D73FD`). The head is
the 16x15 part: its colour tile is `fmt=0 siz=2 line=4 sample=16x15` and its mask
is `fmt=4 siz=0 line=1 tmem=408 sample=16x15`. For those tiles the script prints

  [spr]       the draw-call state and the tile descriptor + the resolved hash
  [dec]       the RGBA the renderer decodes out of the TMEM snapshot it hashes
  [gpu-tile]  the texture the cache handed back (idx/dims/found)

Usage: analyze-spr.py <run.log>
"""
import re
import sys
from collections import defaultdict

SPR = re.compile(r"\[spr\] call=(\d+) tile=(\d+)/(\d+) cycle=(\d+) combine=([0-9A-F]+)/([0-9A-F]+) "
                 r"prim=([0-9A-F]+) env=([0-9A-F]+) "
                 r"fmt=(\d+) siz=(\d+) line=(\d+) tmem=(\d+) sample=(\d+)x(\d+) tileCopy=(\d+) raw=(\d+) "
                 r"hash=([0-9A-F]+) tlut=(\d+) rdp\(addr=(-?\d+) stride=(-?\d+) pal=(-?\d+) masks=(-?\d+) maskt=(-?\d+) "
                 r"uls=(-?\d+) lrs=(-?\d+) cmt=(-?\d+) cms=(-?\d+) native=(\d+)\)")
DEC = re.compile(r"\[dec\] hash=([0-9A-F]+) fmt=(\d+) siz=(\d+) line=(\d+) tmem=(\d+) (\d+)x(\d+) "
                 r"cached=(\d+) nz=(\d+)/(\d+) mean=([\d.]+) px00=([0-9A-F]+) pxmid=([0-9A-F]+) pxlast=([0-9A-F]+)")
GPU = re.compile(r"\[gpu-tile\] n=(\d+) hash=([0-9A-F]+) tmemHash=([0-9A-F]+) idx=(\d+) dims=(\d+)x(\d+) "
                 r"tcScale=([\d.,]+) sample=(\d+)x(\d+) rawTMEM=(\d+) copy=(\d+) valid=(\d+) replaced=(\d+) "
                 r"fmt=(\d+) siz=(\d+) tmem=(\d+) line=(\d+) found=(\d+)")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if path is None:
        print(__doc__)
        return
    spr = []
    dec = {}
    gpu = defaultdict(list)
    with open(path, errors='replace') as f:
        for line in f:
            m = SPR.search(line)
            if m:
                spr.append(m.groups())
                continue
            m = DEC.search(line)
            if m:
                g = m.groups()
                dec.setdefault(g[0], []).append(g)
                continue
            m = GPU.search(line)
            if m:
                g = m.groups()
                gpu[g[1]].append(g)

    def tile(d, i):
        return dict(call=int(d[0]), tile=int(d[1]), n=int(d[2]), cycle=int(d[3]), combine=d[4] + '/' + d[5],
                    prim=d[6], env=d[7], fmt=int(d[8]), siz=int(d[9]), line=int(d[10]), tmem=int(d[11]),
                    w=int(d[12]), h=int(d[13]), copy=int(d[14]), raw=int(d[15]), hash=d[16],
                    addr=int(d[18]), stride=int(d[19]), native=int(d[27]))

    tiles = [tile(d, i) for i, d in enumerate(spr)]

    # The unit's pass 2: 2-cycle, the head's combiner, and the 16x15 sample.
    heads = [t for t in tiles if t['cycle'] == 1 and t['combine'] == 'FC62FE03/2F0D73FD'
             and t['w'] == 16 and t['h'] == 15]
    print(f"{len(spr)} [spr] lines, {len(dec)} distinct decoded hashes, {len(gpu)} distinct gpu hashes")
    if not heads:
        print("no 2-cycle 16x15 tile found -- the run did not reach the unit")
        return
    for t in heads:
        print(f"\n=== call {t['call']} tile {t['tile']}/{t['n']}  fmt={t['fmt']} siz={t['siz']} line={t['line']} "
              f"tmem={t['tmem']} sample={t['w']}x{t['h']} copy={t['copy']} raw={t['raw']} hash={t['hash']}")
        print(f"    rdp addr={t['addr']} stride={t['stride']} native={t['native']} prim={t['prim']} env={t['env']}")
        for d in dec.get(t['hash'], []):
            print(f"    [dec]  {d[4]}x{d[5]} cached={d[7]} nz={d[8]}/{d[9]} mean={d[10]} px00={d[11]} pxmid={d[12]} pxlast={d[13]}")
        if t['hash'] not in dec:
            print("    [dec]  <no line: the hash was never hashed in this run>")
        for g in gpu.get(t['hash'], []):
            print(f"    [gpu]  idx={g[3]} dims={g[4]}x{g[5]} rawTMEM={g[9]} copy={g[10]} found={g[17]}")
        if t['hash'] not in gpu:
            print("    [gpu]  <no line: the tile never reached the texture cache>")
    # Every distinct tile shape in the unit's pass 2, for the control parts.
    print("\n--- pass 2 tiles by shape")
    seen = set()
    for t in tiles:
        if t['cycle'] != 1 or t['combine'] != 'FC62FE03/2F0D73FD':
            continue
        key = (t['fmt'], t['siz'], t['line'], t['w'], t['h'])
        if key in seen:
            continue
        seen.add(key)
        d = dec.get(t['hash'], [])
        g = gpu.get(t['hash'], [])
        print(f"  call {t['call']:4d} t{t['tile']} fmt={t['fmt']} siz={t['siz']} line={t['line']:2d} "
              f"sample={t['w']:2d}x{t['h']:<2d} copy={t['copy']} raw={t['raw']} hash={t['hash']} "
              f"dec={'nz=%s/%s mean=%s' % (d[0][8], d[0][9], d[0][10]) if d else 'none'} "
              f"gpu={'idx=%s dims=%sx%s found=%s' % (g[0][3], g[0][4], g[0][5], g[0][17]) if g else 'none'}")


if __name__ == '__main__':
    main()
