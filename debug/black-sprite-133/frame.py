"""Render the display list that starts at a given address, as the RDP draws it."""
import sys
from render_char import load, byte, half, png
from compose import decode_rgb, W, H

def word(d,a):
    o=a&0x1FFFFFFF
    return int.from_bytes(d[o:o+4],'big')
def p0(v,pos,bits): return (v>>pos)&((1<<bits)-1)

class T:
    __slots__=('fmt','siz','line','tmem','palette','uls','ult','lrs','lrt')
    def __init__(self): self.fmt=self.siz=self.line=self.tmem=self.palette=0; self.uls=self.ult=self.lrs=self.lrt=0

def render_dl(d, start, limit=4000, bg=(24,24,40)):
    tiles=[T() for _ in range(8)]
    tmem_src={}; tlut={}
    timg=0; tf=0; ts=0; tw=1
    draws=[]
    a=start; n=0
    while n<limit:
        w0=word(d,a); w1=word(d,a+4); op=w0>>24; n+=1
        if op==0xFD:
            timg=w1; tf=p0(w0,21,3); ts=p0(w0,19,2); tw=p0(w0,0,12)+1; a+=8
        elif op==0xF5:
            t=p0(w1,24,3); tl=tiles[t]
            tl.fmt=p0(w0,21,3); tl.siz=p0(w0,19,2); tl.line=p0(w0,9,9); tl.tmem=p0(w0,0,9); tl.palette=p0(w1,20,4)
            a+=8
        elif op==0xF2:
            t=p0(w1,24,3); tl=tiles[t]
            tl.uls=p0(w0,12,12); tl.ult=p0(w0,0,12); tl.lrs=p0(w1,12,12); tl.lrt=p0(w1,0,12); a+=8
        elif op in (0xF3,0xF4):
            t=p0(w1,24,3); tl=tiles[t]
            tmem_src[tl.tmem]=timg; a+=8
        elif op==0xF0:
            t=p0(w1,24,3); tl=tiles[t]
            src=timg+(((p0(w0,12,12))>>2)<<ts>>1)
            tlut[tl.tmem]=src; a+=8
        elif op in (0xE4,0xE5):
            tile=p0(w1,24,3); tl=tiles[tile]
            st=word(d,a+12); ds=word(d,a+20)
            s=(st>>16)&0xFFFF; t=st&0xFFFF; dsdx=(ds>>16)&0xFFFF; dtdy=ds&0xFFFF
            s=s-0x10000 if s>=0x8000 else s; t=t-0x10000 if t>=0x8000 else t
            dsdx=dsdx-0x10000 if dsdx>=0x8000 else dsdx; dtdy=dtdy-0x10000 if dtdy>=0x8000 else dtdy
            want=0x100+tl.palette*16
            pal=None
            for k in sorted(tlut):
                if k<=want: pal=tlut[k]
            if pal is None and tlut: pal=tlut[min(tlut)]
            draws.append(dict(at=a,tile=tile,fmt=tl.fmt,siz=tl.siz,line=tl.line,palette=tl.palette,
                ulx=p0(w1,12,12)//4,uly=p0(w1,0,12)//4,lrx=p0(w0,12,12)//4,lry=p0(w0,0,12)//4,
                s=s,t=t,dsdx=dsdx,dtdy=dtdy,src=tmem_src.get(tl.tmem, timg),pal=pal))
            a+=24
        elif op==0xDE:
            sub,_=render_dl(d,w1,limit,bg); draws+=sub; a+=8
        elif op==0xDF:
            break
        else:
            a += 16 if op==0x04 else 8
    buf=bytearray(bytes(bg)*W*H)
    for dr in draws:
        t=dict(fmt=dr['fmt'],siz=dr['siz'],line=dr['line'],palette=dr['palette'])
        for y in range(max(0,dr['uly']),min(H,dr['lry'])):
            for x in range(max(0,dr['ulx']),min(W,dr['lrx'])):
                tsx=(dr['s']+dr['dsdx']*(x-dr['ulx'])//32)//32
                ttx=(dr['t']+dr['dtdy']*(y-dr['uly'])//32)//32
                px=decode_rgb(d,dr['src'],dr['pal'] or 0,t,max(tsx,0),max(ttx,0))
                if px is None or px[3]==0: continue
                o=(y*W+x)*3; buf[o]=px[0]; buf[o+1]=px[1]; buf[o+2]=px[2]
    return draws, buf

if __name__=='__main__':
    path, start, out = sys.argv[1], int(sys.argv[2],0), sys.argv[3]
    d=load(path)
    draws,buf=render_dl(d,start)
    png(out,W,H,buf)
    print(out,'draws',len(draws))
    for dr in draws[:6]:
        print('  %08X rect=%d,%d..%d,%d tile=%d fmt=%d siz=%d line=%d pal=%d src=%08X paladdr=%s'%(
            dr['at'],dr['ulx'],dr['uly'],dr['lrx'],dr['lry'],dr['tile'],dr['fmt'],dr['siz'],dr['line'],dr['palette'],dr['src'],hex(dr['pal']) if dr['pal'] else None))
