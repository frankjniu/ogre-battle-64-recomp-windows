"""Walk the display lists inside a whole-RDRAM dump and report every draw with
the RDRAM image it samples, using the RDP's own tile/TMEM rules.

ROM order: file byte at (a & 0x1FFFFFFF) is the logical byte at guest a.
"""
import sys, collections
from render_char import load, byte, half, png

def word(d,a):
    o=a&0x1FFFFFFF
    return int.from_bytes(d[o:o+4],'big')
def p0(v,pos,bits): return (v>>pos)&((1<<bits)-1)

class Tile:
    def __init__(self):
        self.fmt=0; self.siz=0; self.line=0; self.tmem=0; self.palette=0
        self.uls=0; self.ult=0; self.lrs=0; self.lrt=0

class Walker:
    def __init__(self, d):
        self.d=d
        self.reset()
    def reset(self):
        self.tiles=[Tile() for _ in range(8)]
        self.tmem_src={}          # tmem word offset -> (addr, siz, width)
        self.tlut_src={}          # tmem word offset -> addr (palette)
        self.timg=0; self.timg_fmt=0; self.timg_siz=0; self.timg_w=1
        self.draws=[]
    def run(self, start, limit=400):
        d=self.d
        a=start; n=0
        while n<limit:
            w0=word(d,a); w1=word(d,a+4); op=w0>>24
            n+=1
            if op==0xFD:
                self.timg=w1; self.timg_fmt=p0(w0,21,3); self.timg_siz=p0(w0,19,2); self.timg_w=p0(w0,0,12)+1
                a+=8
            elif op==0xF5:
                t=p0(w1,24,3); tl=self.tiles[t]
                tl.fmt=p0(w0,21,3); tl.siz=p0(w0,19,2); tl.line=p0(w0,9,9); tl.tmem=p0(w0,0,9); tl.palette=p0(w1,20,4)
                a+=8
            elif op==0xF2:
                t=p0(w1,24,3); tl=self.tiles[t]
                tl.uls=p0(w0,12,12); tl.ult=p0(w0,0,12); tl.lrs=p0(w1,12,12); tl.lrt=p0(w1,0,12)
                a+=8
            elif op==0xF3 or op==0xF4:   # LOADBLOCK / LOADTILE
                t=p0(w1,24,3); tl=self.tiles[t]
                tl.uls=p0(w0,12,12); tl.ult=p0(w0,0,12)
                if op==0xF3:
                    tl.lrs=p0(w1,12,12); tl.lrt=p0(w1,0,12)
                else:
                    tl.lrs=p0(w1,12,12); tl.lrt=p0(w1,0,12)
                self.tmem_src[tl.tmem]=(self.timg, self.timg_siz, self.timg_w, op, tl.line)
                a+=8
            elif op==0xF0:               # LOADTLUT
                t=p0(w1,24,3); tl=self.tiles[t]
                src=self.timg + (( (p0(w0,12,12)) >>2) << self.timg_siz >> 1)
                self.tlut_src[tl.tmem]=(src, p0(w1,14,10)+1)
                a+=8
            elif op in (0xE4,0xE5):
                tile=p0(w1,24,3)
                st=word(d,a+12); ds=word(d,a+20)
                ulx=p0(w1,12,12); uly=p0(w1,0,12); lrx=p0(w0,12,12); lry=p0(w0,0,12)
                s=(st>>16)&0xFFFF; t=st&0xFFFF
                s = s-0x10000 if s>=0x8000 else s
                t = t-0x10000 if t>=0x8000 else t
                dsdx=(ds>>16)&0xFFFF; dtdy=ds&0xFFFF
                dsdx = dsdx-0x10000 if dsdx>=0x8000 else dsdx
                dtdy = dtdy-0x10000 if dtdy>=0x8000 else dtdy
                tl=self.tiles[tile]
                src=self.tmem_src.get(tl.tmem)
                pal=self.tlut_src.get(tl.tmem)
                self.draws.append(dict(at=a, op=op, tile=tile, fmt=tl.fmt, siz=tl.siz, line=tl.line,
                    tmem=tl.tmem, palette=tl.palette, uls=tl.uls, ult=tl.ult, lrs=tl.lrs, lrt=tl.lrt,
                    ulx=ulx//4, uly=uly//4, lrx=lrx//4, lry=lry//4, s=s, t=t, dsdx=dsdx, dtdy=dtdy,
                    src=src, pal=pal))
                a+=24
            elif op==0xDE:
                self.run(w1, limit)
                a+=8
            elif op==0xDF:
                return
            else:
                a += 16 if op==0x04 else 8
