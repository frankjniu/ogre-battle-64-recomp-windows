"""Extract the S2DEX2 object draws (G_OBJ_MOVEMEM / G_OBJ_RECTANGLE_R) from a dump."""
import sys

def p0(v,pos,bits): return (v>>pos)&((1<<bits)-1)
def s16(v): return v-0x10000 if v>=0x8000 else v

class D:
    def __init__(self, path):
        self.d=open(path,'rb').read()
    def w(self,a):
        o=a&0x1FFFFFFF
        return int.from_bytes(self.d[o:o+4],'big')
    def bytes(self,a,n):
        o=a&0x1FFFFFFF
        return self.d[o:o+n]

def decode_sprite(d, addr):
    w=[d.w(addr+4*i) for i in range(6)]
    s={}
    s['objX']=s16(w[0]>>16); s['scaleW']=w[0]&0xFFFF
    s['imageW']=w[1]>>16
    s['objY']=s16(w[2]>>16); s['scaleH']=w[2]&0xFFFF
    s['imageH']=w[3]>>16
    s['imageStride']=w[4]>>16; s['imageAdrs']=w[4]&0xFFFF
    s['imageFmt']=(w[5]>>24)&0xFF; s['imageSiz']=(w[5]>>16)&0xFF
    s['imagePal']=(w[5]>>8)&0xFF; s['imageFlags']=w[5]&0xFF
    s['raw']=w
    return s

def walk(d, start, out, limit=4000, depth=0):
    tiles=[dict(fmt=0,siz=0,line=0,tmem=0,pal=0,uls=0,ult=0,lrs=0,lrt=0) for _ in range(8)]
    tmem_src={}
    mtx=dict(a=0x10000,b=0,c=0,d=0x10000,x=0,y=0,baseScaleX=1024,baseScaleY=1024,sub=False)
    timg=0; tf=0; ts=0; tw=1
    a=start; n=0
    while n<limit:
        w0=d.w(a); w1=d.w(a+4); op=w0>>24
        n+=1
        if op==0xFD:
            timg=w1; tf=p0(w0,21,3); ts=p0(w0,19,2); tw=p0(w0,0,12)+1; a+=8
        elif op==0xF5:
            t=tiles[p0(w1,24,3)]
            t.update(fmt=p0(w0,21,3),siz=p0(w0,19,2),line=p0(w0,9,9),tmem=p0(w0,0,9),pal=p0(w1,20,4))
            a+=8
        elif op==0xF2:
            t=tiles[p0(w1,24,3)]
            t.update(uls=p0(w0,12,12),ult=p0(w0,0,12),lrs=p0(w1,12,12),lrt=p0(w1,0,12)); a+=8
        elif op in (0xF3,0xF4):
            t=tiles[p0(w1,24,3)]
            t.update(uls=p0(w0,12,12),ult=p0(w0,0,12),lrs=p0(w1,12,12),lrt=p0(w1,0,12))
            tmem_src[t['tmem']]=dict(addr=timg,fmt=tf,siz=ts,w=tw,line=t['line'],kind=op)
            a+=8
        elif op==0xF0:
            t=tiles[p0(w1,24,3)]
            a+=8
        elif op==0xDC:   # G_OBJ_MOVEMEM
            idx=w0&0xFFFF
            p=w1
            if idx==0:
                mtx=dict(a=d.w(p),b=d.w(p+4),c=d.w(p+8),d=d.w(p+12),x=s16(d.w(p+16)>>16),y=s16(d.w(p+16)&0xFFFF),
                         baseScaleX=d.w(p+20)>>16,baseScaleY=d.w(p+20)&0xFFFF,sub=False)
            elif idx==2:
                mtx=dict(a=0x10000,b=0,c=0,d=0x10000,x=s16(d.w(p)>>16),y=s16(d.w(p)&0xFFFF),
                         baseScaleX=d.w(p+4)>>16,baseScaleY=d.w(p+4)&0xFFFF,sub=True)
            a+=8
        elif op==0xDA:   # G_OBJ_RECTANGLE_R
            spr=decode_sprite(d, w1)
            out.append(dict(at=a,spr=spr,mtx=dict(mtx),src=dict(tmem_src.get(spr['imageAdrs'], {})) ,tiles=[dict(t) for t in tiles]))
            a+=8
        elif op==0xDE:
            if depth<6: walk(d,w1,out,limit,depth+1)
            a+=8
        elif op==0xDF:
            return out
        elif op in (0xE4,0xE5):
            a+=24
        elif op==0x04:
            a+=16
        else:
            a+=8
    return out

if __name__=='__main__':
    path=sys.argv[1]; root=int(sys.argv[2],0)
    d=D(path)
    out=walk(d,root,[])
    print(path,'objects:',len(out))
    for o in out[:24]:
        s=o['spr']; m=o['mtx']; src=o['src']
        print('  %08X obj=(%d,%d) scale=(%d,%d) img=%dx%d stride=%d adrs=%d fmt=%d siz=%d pal=%d flags=%02X mtx=(%.2f,%.2f,%.3f,%.3f) src=%s' % (
            o['at'], s['objX'], s['objY'], s['scaleW'], s['scaleH'], s['imageW'], s['imageH'], s['imageStride'], s['imageAdrs'],
            s['imageFmt'], s['imageSiz'], s['imagePal'], s['imageFlags'],
            m['x']/4.0, m['y']/4.0, max(m['baseScaleX'],1)/1024.0, max(m['baseScaleY'],1)/1024.0,
            ('0x%08X f%d s%d w%d line%d'%(src['addr'],src['fmt'],src['siz'],src['w'],src['line'])) if src else '-'))
