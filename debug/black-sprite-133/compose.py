"""Composite every TEXRECT in a dump's display lists into a 320x240 RGB frame."""
import sys, collections
from render_char import load, byte, half, png
from dlwalk import Walker, word

W,H = 320,240

def decode_rgb(d, src, pal_addr, tile, tex_s, tex_t):
    """Return (r,g,b,opaque) for one texel."""
    fmt, siz, line = tile['fmt'], tile['siz'], tile['line']
    bpr = (line*8) if line else 1
    if fmt == 2:
        if siz == 0:      # CI4
            b = byte(d, src + tex_t*bpr + (tex_s >> 1))
            idx = (b >> 4) if (tex_s & 1) == 0 else (b & 0xF)
            idx += tile['palette']*16
        else:             # CI8
            idx = byte(d, src + tex_t*bpr + tex_s) + tile['palette']*16
        c = half(d, pal_addr + idx*2)
        return (((c>>11)&31)*255//31, ((c>>6)&31)*255//31, ((c>>1)&31)*255//31, c & 1)
    if fmt == 0:
        if siz == 2:      # RGBA16
            c = half(d, src + tex_t*bpr + tex_s*2)
            return (((c>>11)&31)*255//31, ((c>>6)&31)*255//31, ((c>>1)&31)*255//31, c & 1)
        if siz == 3:      # RGBA32
            o = src + tex_t*bpr + tex_s*4
            return (byte(d,o), byte(d,o+1), byte(d,o+2), 1)
    return None

def composite(d, draws, bg=(0,0,0)):
    buf = bytearray(bytes(bg)*W*H)
    for dr in draws:
        if dr['src'] is None:
            continue
        if dr['ulx'] >= W or dr['uly'] >= H or dr['lrx'] <= 0 or dr['lry'] <= 0:
            continue
        t = dict(fmt=dr['fmt'], siz=dr['siz'], line=dr['line'], palette=dr['palette'])
        for y in range(dr['uly'], dr['lry']):
            if y < 0 or y >= H: continue
            for x in range(dr['ulx'], dr['lrx']):
                if x < 0 or x >= W: continue
                ts = (dr['s'] + dr['dsdx']*(x-dr['ulx'])//32)//32
                tt = (dr['t'] + dr['dtdy']*(y-dr['uly'])//32)//32
                if ts < 0: ts = 0
                if tt < 0: tt = 0
                px = decode_rgb(d, dr['src'][0], dr['pal'][0] if dr['pal'] else 0, t, ts, tt)
                if px is None or px[3] == 0:
                    continue
                o=(y*W+x)*3
                buf[o]=px[0]; buf[o+1]=px[1]; buf[o+2]=px[2]
    return buf

if __name__ == '__main__':
    path, out = sys.argv[1], sys.argv[2]
    seed0 = int(sys.argv[3],0) if len(sys.argv)>3 else None
    d = load(path)
    # seeds: every SETTIMG (0xFD) word in the dump
    seeds=[]
    for o in range(0,len(d)-8,8):
        if d[o]==0xFD:
            seeds.append(0x80000000+o)
    w = Walker(d)
    seen={}
    for s in seeds:
        w.reset(); w.run(s, 80)
        for dr in w.draws:
            # resolve palette from tmem: palettes live at tmem 0x100 + 16*P
            want = 0x100 + dr['palette']*16
            pal = None
            if dr['pal'] and dr['pal'][0]:
                # the walker's tlut_src keyed by tile tmem; find best match
                pass
            if dr['at'] not in seen:
                seen[dr['at']] = dr
    draws = [seen[k] for k in sorted(seen)]
    # palette resolution pass: re-walk to capture tlut per draw using draw tile's palette
    print('draws', len(draws))
    buf = composite(d, draws)
    png(out, W, H, buf)
    print('wrote', out)
