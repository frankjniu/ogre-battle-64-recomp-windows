"""Minimal PNG writer (stdlib zlib only), for looking at dumped framebuffers."""
import struct, zlib

def write_png(path, w, h, rgb):
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y*stride:(y+1)*stride]
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)

def half(d, a):
    a &= 0x1FFFFFFF
    return int.from_bytes(d[a:a+2], 'big')

def fb_to_png(dump, addr, out, scale=1, crop=None):
    """addr: guest address of a 320x240 RGBA5551 framebuffer. crop=(x,y,w,h)."""
    d = dump
    x0, y0, cw, ch = crop if crop else (0, 0, 320, 240)
    rgb = bytearray(cw*scale*ch*scale*3)
    for y in range(ch):
        for x in range(cw):
            c = half(d, addr + ((y+y0)*320 + (x+x0))*2)
            r = ((c >> 11) & 31) * 255 // 31
            g = ((c >> 6) & 31) * 255 // 31
            b = ((c >> 1) & 31) * 255 // 31
            for sy in range(scale):
                base = (((y*scale+sy)*cw*scale) + x*scale)*3
                for sx in range(scale):
                    o = base + sx*3
                    rgb[o] = r; rgb[o+1] = g; rgb[o+2] = b
    write_png(out, cw*scale, ch*scale, rgb)
    return out
