import sys, zlib, struct
from render_char import png

def load(p): return open(p,'rb').read()
def gb(d,a): return d[a & 0x1FFFFFFF]
def hw(d,a): return int.from_bytes(bytes(gb(d,a+i) for i in range(2)),'big')

def records(d):
    out=[]
    for o in range(0,0x800000-8,2):
        if d[o]==0x36 and d[o+1]==0x34:
            flags=int.from_bytes(d[o+2:o+4],'big')
            w=int.from_bytes(d[o+4:o+6],'big'); h=int.from_bytes(d[o+6:o+8],'big')
            if 1<=w<=64 and 1<=h<=64:
                out.append((0x80000000+o,flags,w,h))
    return out

def render(d, addr, flags, w, h, scale, out):
    payload=addr+8
    rgb=bytearray(w*scale*h*scale*3)
    for y in range(h):
        for x in range(w):
            if flags==0x0002:      # RGBA16
                c=hw(d,payload+(y*w+x)*2)
                r=((c>>11)&31)*255//31; g=((c>>6)&31)*255//31; b=((c>>1)&31)*255//31; a=c&1
            else:                  # I8
                v=gb(d,payload+y*w+x)
                r=g=b=v; a=1 if v else 0
            if not a:
                r=g=b=40      # mark transparent as grey
            for sy in range(scale):
                for sx in range(scale):
                    o=(((y*scale+sy)*w*scale)+(x*scale+sx))*3
                    rgb[o]=r; rgb[o+1]=g; rgb[o+2]=b
    png(out,w*scale,h*scale,rgb)

if __name__=='__main__':
    src,scale,prefix=sys.argv[1],int(sys.argv[2]),sys.argv[3]
    d=load(src)
    recs=records(d)
    print(src,'records',len(recs))
    for i,(a,flags,w,h) in enumerate(recs):
        # nonzero count of the payload
        bpp=2 if flags==0x0002 else 1
        n=w*h*bpp
        nz=sum(1 for k in range(n) if gb(d,a+8+k))
        print('  %2d 0x%08X flags=%04X %2dx%-3d nz=%d/%d'%(i,a,flags,w,h,nz,n))
        render(d,a,flags,w,h,scale,'%s-%02d-%04X.png'%(prefix,i,flags))
