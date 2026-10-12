import zlib, struct, sys

def png(path, w, h, rgb):
    raw = b''.join(b'\x00' + bytes(rgb[y*w*3:(y+1)*w*3]) for y in range(h))
    def chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    open(path,'wb').write(b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

def load(p):
    return open(p,'rb').read()

def byte(d, a):
    return d[a & 0x1FFFFFFF]

def half(d, a):
    o = a & 0x1FFFFFFE
    return int.from_bytes(d[o:o+2],'big')

def render(d, img, pal, w, h, line, scale=1, tag=''):
    # CI8: one index byte per texel, row stride line*8 bytes
    bpr = line*8 if line else w
    rgb = bytearray(w*scale*h*scale*3)
    for y in range(h):
        for x in range(w):
            idx = byte(d, img + y*bpr + x)
            c = half(d, pal + idx*2)
            r = ((c >> 11) & 31) * 255 // 31
            g = ((c >> 6) & 31) * 255 // 31
            b = ((c >> 1) & 31) * 255 // 31
            for sy in range(scale):
                for sx in range(scale):
                    o = (((y*scale+sy)*w*scale) + (x*scale+sx))*3
                    rgb[o:o+3] = bytes((r,g,b))
    return rgb

if __name__ == '__main__':
    path, img, pal, w, h, line, out, scale = sys.argv[1], int(sys.argv[2],0), int(sys.argv[3],0), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]), sys.argv[7], int(sys.argv[8])
    d = load(path)
    rgb = render(d, img, pal, w, h, line, scale)
    png(out, w*scale, h*scale, rgb)
    print('wrote', out)
