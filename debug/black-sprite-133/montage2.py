import sys
from render_char import png
from parts import load, records, gb, hw

CELL=78
def cell_for(d,addr,flags,w,h):
    scale=max(1,min(6,64//max(w,h)))
    cell=bytearray(bytes((20,20,30))*CELL*CELL)
    for y in range(h*scale):
        for x in range(w*scale):
            sx,sy=x//scale,y//scale
            if flags==0x0002:
                c=hw(d,addr+8+(sy*w+sx)*2)
                r=((c>>11)&31)*255//31; g=((c>>6)&31)*255//31; b=((c>>1)&31)*255//31; a=c&1
            else:
                v=gb(d,addr+8+sy*w+sx); r=g=b=v; a=1 if v else 0
            if not a: r,g,b=60,60,80
            o=((y+7)*CELL+x+7)*3
            if 0<=y+7<CELL and 0<=x+7<CELL:
                cell[o]=r; cell[o+1]=g; cell[o+2]=b
    return cell

src,out=sys.argv[1],sys.argv[2]
d=load(src); recs=records(d)
COLS=6; rows=(len(recs)+COLS-1)//COLS
W=COLS*CELL; H=rows*CELL
buf=bytearray(bytes((20,20,30))*W*H)
for i,(a,flags,w,h) in enumerate(recs):
    c=cell_for(d,a,flags,w,h)
    cx=(i%COLS)*CELL; cy=(i//COLS)*CELL
    for y in range(CELL):
        o1=y*CELL*3; o2=((cy+y)*W+cx)*3
        buf[o2:o2+CELL*3]=c[o1:o1+CELL*3]
png(out,W,H,buf); print(out,W,H)
