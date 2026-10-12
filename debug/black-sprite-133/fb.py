import sys
from render_char import png
def load(p): return open(p,'rb').read()
def hw(d,a): return int.from_bytes(bytes(d[(a&0x1FFFFFFF)+i] for i in range(2)),'big')
src,addr,out,scale=sys.argv[1],int(sys.argv[2],0),sys.argv[3],int(sys.argv[4])
d=load(src); W,H=320,240
buf=bytearray(W*scale*H*scale*3)
for y in range(H):
    for x in range(W):
        c=hw(d,addr+(y*W+x)*2)
        r=((c>>11)&31)*255//31; g=((c>>6)&31)*255//31; b=((c>>1)&31)*255//31
        for sy in range(scale):
            for sx in range(scale):
                o=(((y*scale+sy)*W*scale)+(x*scale+sx))*3
                buf[o]=r;buf[o+1]=g;buf[o+2]=b
png(out,W*scale,H*scale,buf); print(out)
