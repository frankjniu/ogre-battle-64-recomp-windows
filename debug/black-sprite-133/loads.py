"""Every texture load in a frame's display list, with the RDRAM it reads."""
import sys

def p0(v,pos,bits): return (v>>pos)&((1<<bits)-1)

class D:
    def __init__(self, path): self.d=open(path,'rb').read()
    def w(self,a):
        o=a&0x1FFFFFFF
        return int.from_bytes(self.d[o:o+4],'big')
    def nz(self,a,n):
        o=a&0x1FFFFFFF
        if o+n>0x800000: return None
        seg=self.d[o:o+n]
        return sum(1 for x in seg if x), n

def walk(d, start, loads, limit=6000, depth=0):
    timg=0; tf=0; ts=0; tw=1
    a=start; n=0
    while n<limit:
        w0=d.w(a); w1=d.w(a+4); op=w0>>24
        n+=1
        if op==0xFD:
            timg=w1; tf=p0(w0,21,3); ts=p0(w0,19,2); tw=p0(w0,0,12)+1; a+=8
        elif op==0xF5:
            tile=p0(w1,24,3)
            loads.append(dict(kind='SETTILE',at=a,tile=tile,fmt=p0(w0,21,3),siz=p0(w0,19,2),line=p0(w0,9,9),tmem=p0(w0,0,9),pal=p0(w1,20,4)))
            a+=8
        elif op in (0xF3,0xF4):
            tile=p0(w1,24,3)
            uls=p0(w0,12,12); ult=p0(w0,0,12); lrs=p0(w1,12,12); lrt=p0(w1,0,12)
            if op==0xF4:
                bpr = (tw << ts) >> 1
                start_b = timg + ((uls>>2) << ts >> 1) + bpr*(ult>>2)
                rows = 1 + ((lrt>>2)-(ult>>2))
                words = ((lrs>>2)-(uls>>2)) >> (4-ts)
                words = (words + 1) if words>=0 else 1
                size = (rows-1)*bpr + words*8
            else:
                bpr = (tw << ts) >> 1
                start_b = timg + ((uls) << ts >> 1) + bpr*ult
                words = ((lrs-uls) >> (4-ts)) + 1
                size = words*8
            st = d.nz(start_b, max(size,1))
            loads.append(dict(kind='LOADBLOCK' if op==0xF3 else 'LOADTILE',at=a,tile=tile,uls=uls,ult=ult,lrs=lrs,lrt=lrt,
                              timg=timg,fmt=tf,siz=ts,w=tw,bpr=bpr,src=start_b,size=size,nonzero=st[0] if st else None))
            a+=8
        elif op==0xDE:
            if depth<6: walk(d,w1,loads,limit,depth+1)
            a+=8
        elif op==0xDF:
            return loads
        elif op in (0xE4,0xE5):
            a+=24
        elif op==0x04:
            a+=16
        else:
            a+=8
    return loads

if __name__=='__main__':
    path=sys.argv[1]; root=int(sys.argv[2],0)
    d=D(path)
    loads=walk(d,root,[])
    lt=[l for l in loads if l['kind'] in ('LOADTILE','LOADBLOCK')]
    print(path,'loads:',len(lt))
    zero=[l for l in lt if l['nonzero']==0]
    print('loads whose source is ALL ZERO:',len(zero))
    for l in zero[:20]:
        print('   %08X %s src=0x%08X size=%d tile=%d imgfmt=%d imgsiz=%d w=%d line=%d' % (l['at'],l['kind'],l['src'],l['size'],l['tile'],l['fmt'],l['siz'],l['w'],l.get('line',0)))
    print()
    print('all loads (first 30):')
    for l in lt[:30]:
        print('   %08X %s src=0x%08X size=%-6d nz=%-6s fmt=%d siz=%d w=%-4d uls=%d ult=%d lrs=%d lrt=%d' % (
            l['at'],l['kind'],l['src'],l['size'],l['nonzero'],l['fmt'],l['siz'],l['w'],l['uls'],l['ult'],l['lrs'],l['lrt']))
