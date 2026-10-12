import zlib, struct, sys

def read_png(path):
    d = open(path,'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    pos = 8; idat = b''; w=h=bd=ct=None; plte=None; trns=None
    while pos < len(d):
        ln = struct.unpack_from('>I', d, pos)[0]; typ = d[pos+4:pos+8]
        data = d[pos+8:pos+8+ln]; pos += 12+ln
        if typ==b'IHDR':
            w,h,bd,ct,comp,filt,inter = struct.unpack('>IIBBBBB', data)
            assert inter==0, 'interlaced unsupported'
        elif typ==b'IDAT': idat += data
        elif typ==b'PLTE': plte = data
        elif typ==b'tRNS': trns = data
        elif typ==b'IEND': break
    raw = zlib.decompress(idat)
    ch = {0:1,2:3,3:1,4:2,6:4}[ct]
    assert bd==8, f'bitdepth {bd}'
    stride = w*ch
    out = bytearray(h*stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p+=1
        line = bytearray(raw[p:p+stride]); p+=stride
        if f==1:
            for i in range(ch, stride): line[i]=(line[i]+line[i-ch])&0xFF
        elif f==2:
            for i in range(stride): line[i]=(line[i]+prev[i])&0xFF
        elif f==3:
            for i in range(stride):
                a = line[i-ch] if i>=ch else 0
                line[i]=(line[i]+((a+prev[i])>>1))&0xFF
        elif f==4:
            for i in range(stride):
                a = line[i-ch] if i>=ch else 0
                b = prev[i]; c = prev[i-ch] if i>=ch else 0
                pa=abs(b-c); pb=abs(a-c); pc=abs(a+b-2*c)
                pr = a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                line[i]=(line[i]+pr)&0xFF
        out[y*stride:(y+1)*stride]=line
        prev=line
    return w,h,ch,ct,plte,trns,bytes(out)

if __name__=='__main__':
    w,h,ch,ct,plte,trns,px = read_png(sys.argv[1])
    print(f'{w}x{h} ch={ch} colortype={ct}')
    if ct==3 and plte:
        print('palette', len(plte)//3, 'entries')
    # print as a grid of hex grey/colour values
    for y in range(0,h,4):
        row=[]
        for x in range(0,w,2):
            i=(y*w+x)*ch
            if ct==3:
                row.append('%02x'%px[i])
            elif ct==2:
                row.append('%02x%02x%02x'%tuple(px[i:i+3]))
            elif ct==6:
                row.append('%02x%02x%02x'%tuple(px[i:i+3]))
        print(' '.join(row))
