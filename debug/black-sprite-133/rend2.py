import sys
from render_char import png
def load(p): return open(p,'rb').read()
def gb(d,a): return d[a&0x1FFFFFFF]
def hw(d,a): return int.from_bytes(bytes(gb(d,a+i) for i in range(2)),'big')
def one(d,addr,w,rows,fmt,scale):
    rgb=bytearray(w*scale*rows*scale*3)
    for y in range(rows):
        for x in range(w):
            if fmt=='rgba16':
                c=hw(d,addr+(y*w+x)*2)
                r=((c>>11)&31)*255//31; g=((c>>6)&31)*255//31; b=((c>>1)&31)*255//31; a=c&1
            else:
                v=gb(d,addr+y*w+x); r=g=b=v; a=1 if v else 0
            if not a: r,g,b=20,20,30
            for sy in range(scale):
                for sx in range(scale):
                    o=(((y*scale+sy)*w*scale)+(x*scale+sx))*3
                    rgb[o]=r;rgb[o+1]=g;rgb[o+2]=b
    return rgb,w*scale,rows*scale
def sheet(d,specs,out,scale=6):
    imgs=[one(d,a,w,r,f,scale) for (a,w,r,f) in specs]
    W=sum(i[1]+6 for i in imgs); H=max(i[2] for i in imgs)
    buf=bytearray(bytes((20,20,30))*W*H); xo=0
    for rgb,w,h in imgs:
        for y in range(h):
            o1=y*w*3; o2=(y*W+xo+3)*3
            buf[o2:o2+w*3]=rgb[o1:o1+w*3]
        xo+=w+6
    png(out,W,H,buf); print(out,W,H)
if __name__=='__main__':
    src,out=sys.argv[1],sys.argv[2]
    d=load(src)
    # good frame head part candidates: 16x16 RGBA16 @802C6BE8, 12-wide I8 @802C6DF0,
    # 30x44 RGBA16 @802C6F58, 30-wide I8 @802C7A60
    sheet(d,[(0x802C6BE8,16,30,'rgba16'),(0x802C6DF0,12,40,'i8'),
             (0x802C6F58,30,44,'rgba16'),(0x802C7A60,30,44,'i8')],out,4)
