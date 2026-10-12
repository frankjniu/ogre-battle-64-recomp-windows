"""Offline model of RT64's RDP load-into-TMEM and texel sampling.

Byte order: the whole-image dump writes the guest logical byte at file offset
`o` (app/src/ram_key.cpp), so the word at guest `a` is
int.from_bytes(dump[a & 0x1FFFFFFF : +4], 'big').

RT64's RDRAM_raw[o] = logical byte at o, and `RDP::loadWord` copies
TMEM[j ^ xor] = RDRAM_raw[(src + i) ^ 3], so with this dump
TMEM[j ^ xor] = dump[src + i]: TMEM ends up in guest logical order.
"""
import sys, hashlib, struct

def load(path):
    with open(path, 'rb') as f:
        return f.read()

class Mem:
    def __init__(self, d):
        self.d = d
    def byte(self, a):
        o = a & 0x1FFFFFFF
        if o >= len(self.d):
            raise IndexError(f"guest 0x{a:08X} -> file 0x{o:X} outside image")
        return self.d[o]
    def word(self, a):
        o = a & 0x1FFFFFFF
        return int.from_bytes(self.d[o:o+4], 'big')
    def half(self, a):
        o = a & 0x1FFFFFFF
        return int.from_bytes(self.d[o:o+2], 'big')

def b(v, pos, bits):
    return (v >> pos) & ((1 << bits) - 1)

class Tile:
    __slots__ = ('fmt','siz','line','tmem','palette','cms','cmt','masks','maskt',
                 'shifts','shiftt','uls','ult','lrs','lrt')
    def __init__(self):
        self.fmt = self.siz = self.line = self.tmem = self.palette = 0
        self.cms = self.cmt = 0
        self.masks = self.maskt = 0
        self.shifts = self.shiftt = 0
        self.uls = self.ult = self.lrs = self.lrt = 0
    def copy(self):
        t = Tile()
        for s in Tile.__slots__:
            setattr(t, s, getattr(self, s))
        return t
    def __repr__(self):
        return (f"<tile fmt={self.fmt} siz={self.siz} line={self.line} tmem={self.tmem} "
                f"pal={self.palette} masks={self.masks} maskt={self.maskt} "
                f"uls={self.uls} ult={self.ult} lrs={self.lrs} lrt={self.lrt}>")

class Timg:
    __slots__ = ('addr','fmt','siz','width')
    def __init__(self, addr=0, fmt=0, siz=0, width=1):
        self.addr=addr; self.fmt=fmt; self.siz=siz; self.width=width
    def copy(self):
        return Timg(self.addr, self.fmt, self.siz, self.width)

TMEM_BYTES = 4096

class Rdp:
    """RT64's RDP::loadTileOperation / loadBlockOperation / loadTLUTOperation plus
    the sampler in TextureDecoder.hlsli."""
    def __init__(self, mem):
        self.m = mem
        self.TMEM = bytearray(TMEM_BYTES)
        self.lastLoadOpByTMEM = [None] * 512   # index = tile.tmem (word address)

    # --- loads -------------------------------------------------------------
    def _loadWordToTMEM(self, tmemAddress, xor, src, i):
        self.TMEM[(tmemAddress + i) ^ xor] = self.m.byte(src + i)   # logical byte

    def _loadToTMEMCommon(self, tile, timg, textureStart, bytesPerRow, tmemStart,
                          tmemStride, wordsPerRow, rowCount, block=False, dxtInc=0):
        tmemXorMask = 0
        dxtCounter = 0
        # tmemAdvance 8 for non-RGBA32, textureAdvance 8 for non-TLUT.
        # RT64's row-skip optimization applies only when rowsToSkip > 0; the row
        # pointers are initialized before the check.
        tmemAddressRow = tmemStart & (TMEM_BYTES - 1)
        textureAddressRow = textureStart
        if tmemStride > 0 and (wordsPerRow * 8) <= tmemStride:
            rowsToSkip = rowCount - (TMEM_BYTES - 1 + tmemStride) // tmemStride
            if rowsToSkip > 0:
                tmemAddressRow = (tmemAddressRow + tmemStride * rowsToSkip) & (TMEM_BYTES - 1)
                textureAddressRow += bytesPerRow * rowsToSkip
                tmemXorMask = (rowsToSkip & 1) << 2
                rowCount -= rowsToSkip
        while rowCount > 0:
            tmemAddress = tmemAddressRow
            textureAddress = textureAddressRow
            for w in range(wordsPerRow):
                if block:
                    dxtCounter += dxtInc
                    while dxtCounter >= 0x800:
                        tmemAddress = (tmemAddress + tmemStride) & (TMEM_BYTES - 1)
                        dxtCounter -= 0x800
                        tmemXorMask ^= 4
                for i in range(8):
                    self._loadWordToTMEM(tmemAddress, tmemXorMask, textureAddress, i)
                textureAddress += 8
                tmemAddress = (tmemAddress + 8) & (TMEM_BYTES - 1)
            tmemAddressRow = (tmemAddressRow + tmemStride) & (TMEM_BYTES - 1)
            textureAddressRow += bytesPerRow
            tmemXorMask ^= 4
            rowCount -= 1

    def loadTile(self, tile, timg):
        bytesOffset = (tile.uls >> 2) << timg.siz >> 1
        bytesPerRow = timg.width << timg.siz >> 1
        textureStart = timg.addr + bytesOffset + bytesPerRow * (tile.ult >> 2)
        rowCount = 1 + ((tile.lrt >> 2) - (tile.ult >> 2))
        tileWidth = ((tile.lrs >> 2) - (tile.uls >> 2))
        wordsPerRow = (tileWidth >> (4 - tile.siz)) + 1
        tmemStart = tile.tmem << 3
        tmemStride = tile.line << 3
        self._loadToTMEMCommon(tile, timg, textureStart, bytesPerRow, tmemStart,
                               tmemStride, wordsPerRow, rowCount)

    def loadBlock(self, tile, timg):
        bytesOffset = tile.uls << timg.siz >> 1
        bytesPerRow = timg.width << timg.siz >> 1
        textureStart = timg.addr + bytesOffset + bytesPerRow * tile.ult
        wordCount = ((tile.lrs - tile.uls) >> (4 - tile.siz)) + 1
        tmemStart = tile.tmem << 3
        tmemStride = tile.line << 3
        self._loadToTMEMCommon(tile, timg, textureStart, bytesPerRow, tmemStart,
                               tmemStride, wordCount, 1, block=True, dxtInc=tile.lrt)

    def loadTLUT(self, tile, timg):
        bytesOffset = (tile.uls >> 2) << timg.siz >> 1
        bytesPerRow = timg.width << timg.siz >> 1
        textureStart = timg.addr + bytesOffset + bytesPerRow * (tile.ult >> 2)
        rowCount = 1 + ((tile.lrt >> 2) - (tile.ult >> 2))
        wordsPerRow = ((tile.lrs >> 2) - (tile.uls >> 2)) + 1
        tmemStart = tile.tmem << 3
        tmemStride = tile.line << 5
        for row in range(rowCount):
            ta = (tmemStart + row * tmemStride) & (TMEM_BYTES - 1)
            sa = textureStart + row * bytesPerRow
            for w in range(wordsPerRow):
                self.TMEM[(ta + w*2) & (TMEM_BYTES-1)] = self.m.byte(sa + w*2)
                self.TMEM[(ta + w*2 + 1) & (TMEM_BYTES-1)] = self.m.byte(sa + w*2 + 1)

    # --- sampler (TextureDecoder.hlsli) ------------------------------------
    def implLoadTMEM(self, relativeAddress, maskAddress, orAddress, oddRow, textureStart, rowSize, tmem=None):
        if tmem is None:
            tmem = self.TMEM
        rowStart = (relativeAddress // rowSize) * rowSize
        wordIndex = (relativeAddress - rowStart) // 4
        swapWordIndex = wordIndex ^ 1
        if oddRow:
            finalAddress = textureStart + rowStart + swapWordIndex * 4 + (relativeAddress & 3)
        else:
            finalAddress = textureStart + relativeAddress
        return tmem[((finalAddress & maskAddress) | orAddress) & 0xFFF]

    def sampleTMEM(self, x, y, siz, fmt, address, stride, tlut, palette, tmem=None):
        if tmem is None:
            tmem = self.TMEM
        oddRow = y & 1
        oddColumn = x & 1
        isRgba32 = (fmt == 0 and siz == 3)
        usesTlut = tlut > 0
        tmemShift = 2 if isRgba32 else siz
        addressMask = 0x7FF if (isRgba32 or usesTlut) else 0xFFF
        pixelAddress = y * stride + ((x << tmemShift) >> 1)
        p0 = self.implLoadTMEM(pixelAddress + 0, addressMask, 0, oddRow, address, stride, tmem)
        p1 = self.implLoadTMEM(pixelAddress + 1, addressMask, 0, oddRow, address, stride, tmem)
        p4 = (p0 >> (0 if oddColumn else 4)) & 0xF
        if usesTlut:
            PAL = 0x800
            if siz == 0:
                palAddr = PAL + (palette << 7) + (p4 << 3)
            else:
                palAddr = PAL + (p0 << 3)
            v = tmem[palAddr & 0xFFF] | (tmem[(palAddr+1) & 0xFFF] << 8)
            return rgba16(v)
        if siz == 0:
            return i4(p4, fmt)
        if siz == 1:
            return i8(p0, fmt)
        if siz == 2:
            v = p1 | (p0 << 8)
            return rgba16(v) if fmt == 0 else (0,0,0,1.0)
        return (0,0,0,1.0)

def rgba16(v):
    r = (v >> 11) & 0x1F; g = (v >> 6) & 0x1F; bl = (v >> 1) & 0x1F; a = v & 1
    return (r/31.0, g/31.0, bl/31.0, float(a))

def i4(p, fmt):
    v = p / 15.0
    return (v, v, v, 1.0)

def i8(p, fmt):
    v = p / 255.0
    return (v, v, v, 1.0)

# --- display-list walk -----------------------------------------------------
OP_NAMES = {
    0x00:'NOOP',0x01:'VTX',0x02:'MODIFYVTX',0x03:'CULLDL',0x04:'BRANCH_Z',0x05:'TRI1',
    0x06:'TRI2',0x07:'QUAD',0xD7:'TEXTURE',0xD8:'POPMTX',0xD9:'GEOMETRYMODE',0xDA:'MTX',
    0xDB:'MOVEWORD',0xDC:'MOVEMEM',0xDD:'LOAD_UCODE',0xDE:'DL',0xDF:'ENDDL',0xE0:'SPNOOP',
    0xE1:'RDPHALF_1',0xE2:'SETOTHERMODE_L',0xE3:'SETOTHERMODE_H',0xE4:'TEXRECT',
    0xE5:'TEXRECTFLIP',0xE6:'RDPLOADSYNC',0xE7:'RDPPIPESYNC',0xE8:'RDPTILESYNC',
    0xE9:'RDPFULLSYNC',0xEA:'SETKEYGB',0xEB:'SETKEYR',0xEC:'SETCONVERT',0xED:'SETSCISSOR',
    0xEE:'SETPRIMDEPTH',0xEF:'RDPSETOTHERMODE',0xF0:'LOADTLUT',0xF2:'SETTILESIZE',
    0xF3:'LOADBLOCK',0xF4:'LOADTILE',0xF5:'SETTILE',0xF6:'FILLRECT',0xF7:'SETFILLCOLOR',
    0xF8:'SETFOGCOLOR',0xF9:'SETBLENDCOLOR',0xFA:'SETPRIMCOLOR',0xFB:'SETENVCOLOR',
    0xFC:'SETCOMBINE',0xFD:'SETTIMG',0xFE:'SETZIMG',0xFF:'SETCIMG',
}

class Walker:
    def __init__(self, mem):
        self.m = mem
        self.tiles = [Tile() for _ in range(8)]
        self.timg = Timg()
        self.textureTile = 0
        self.textureOn = False
        self.textureLevel = 0
        self.othermode = [0, 0]
        self.combine = [0, 0]
        self.env = 0; self.prim = 0
        self.texSc = 0xFFFF; self.texTc = 0xFFFF
        self.geom = 0
        self.draws = []
        self.loads = []
        self.rdp = Rdp(mem)
        self.dl_visited = set()

    def run(self, start, limit=200000, depth=0):
        m = self.m
        a = start; n = 0
        while n < limit:
            w0 = m.word(a); w1 = m.word(a+4); op = w0 >> 24
            n += 1
            if op == 0xFD:      # SETTIMG
                self.timg = Timg(w1, b(w0,21,3), b(w0,19,2), b(w0,0,12)+1)
                a += 8
            elif op == 0xF5:    # SETTILE (F3DEX2: cmt/cms/mask/shift/palette come from w1)
                t = b(w1,24,3); tl = self.tiles[t]
                tl.fmt=b(w0,21,3); tl.siz=b(w0,19,2); tl.line=b(w0,9,9); tl.tmem=b(w0,0,9)
                tl.palette=b(w1,20,4); tl.cmt=b(w1,18,2); tl.cms=b(w1,8,2)
                tl.maskt=b(w1,14,4); tl.masks=b(w1,4,4)
                tl.shiftt=b(w1,10,4); tl.shifts=b(w1,0,4)
                a += 8
            elif op == 0xF2:    # SETTILESIZE
                t = b(w1,24,3); tl = self.tiles[t]
                tl.uls=b(w0,12,12); tl.ult=b(w0,0,12); tl.lrs=b(w1,12,12); tl.lrt=b(w1,0,12)
                a += 8
            elif op in (0xF3, 0xF4):   # LOADBLOCK / LOADTILE
                t = b(w1,24,3); tl = self.tiles[t]
                tl.uls=b(w0,12,12); tl.ult=b(w0,0,12); tl.lrs=b(w1,12,12); tl.lrt=b(w1,0,12)
                snap_tile = tl.copy(); snap_timg = self.timg.copy()
                kind = 'block' if op == 0xF3 else 'tile'
                self.loads.append((a, kind, snap_tile, snap_timg))
                if kind == 'block':
                    self.rdp.loadBlock(snap_tile, snap_timg)
                else:
                    self.rdp.loadTile(snap_tile, snap_timg)
                self.rdp.lastLoadOpByTMEM[snap_tile.tmem] = (kind, snap_tile, snap_timg)
                a += 8
            elif op == 0xF0:    # LOADTLUT
                t = b(w1,24,3); tl = self.tiles[t]
                snap = tl.copy(); st = self.timg.copy()
                self.loads.append((a, 'tlut', snap, st))
                self.rdp.loadTLUT(snap, st)
                a += 8
            elif op == 0xD9:    # GEOMETRYMODE
                clr = w0 & 0xFFFFFF
                self.geom = (self.geom & ~clr) | (w1 & clr)
                a += 8
            elif op == 0xD7:    # TEXTURE
                self.textureLevel = b(w0,11,3); self.textureTile = b(w0,8,3)
                self.textureOn = b(w0,1,1) == 1
                self.texSc = b(w1,16,16); self.texTc = b(w1,0,16)
                a += 8
            elif op == 0xFA:    # SETPRIMCOLOR
                self.prim = w1
                a += 8
            elif op == 0xFB:    # SETENVCOLOR
                self.env = w1
                a += 8
            elif op == 0xFC:    # SETCOMBINE
                self.combine = [w0, w1]
                a += 8
            elif op == 0xE2 or op == 0xE3:
                self.othermode[0 if op == 0xE3 else 1] = w1
                a += 8
            elif op == 0xEF:
                self.othermode = [w0 & 0xFFFFFF, w1]
                a += 8
            elif op in (0x05, 0x06, 0x07):
                self.draws.append(dict(
                    at=a, op=op, tiles=[self.tiles[(self.textureTile + k) % 8].copy() for k in range(2)],
                    combine=list(self.combine), prim=self.prim, env=self.env, geom=self.geom,
                    textureOn=self.textureOn, textureTile=self.textureTile,
                    othermode=list(self.othermode), sc=self.texSc, tc=self.texTc,
                    tmem=bytes(self.rdp.TMEM),
                    lastLoad=list(self.rdp.lastLoadOpByTMEM)))
                # F3DEX2 G_TRI1 is one word; G_TRI2 and G_QUAD are two.
                a += 8 if op == 0x05 else 16
            elif op == 0xDE:    # G_DL
                if w1 not in self.dl_visited and depth < 8:
                    self.dl_visited.add(w1)
                    self.run(w1, limit, depth+1)
                a += 8
            elif op == 0xDF:
                return
            elif op == 0x01:    # VTX: F3DEX2 keeps the vertices in a separate array
                a += 8
            else:
                a += 8

def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    path = sys.argv[1]
    root = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x802E2490
    mem = Mem(load(path))
    w = Walker(mem)
    w.run(root)
    print(f"{path}: {len(w.draws)} triangles, {len(w.loads)} loads")
    # Report every triangle whose combiner is the head's and which samples tmem 0/408.
    for i, d in enumerate(w.draws):
        t1 = d['tiles'][1]
        t0 = d['tiles'][0]
        print(f"draw {i:3d} at 0x{d['at']:08X} op={OP_NAMES[d['op']]} textureTile={d['textureTile']} "
              f"on={int(d['textureOn'])} geom=0x{d['geom']:X} comb={d['combine'][0]:08X}/{d['combine'][1]:08X} "
              f"prim={d['prim']:08X} env={d['env']:08X} | t0 {t0} | t1 {t1}")

if __name__ == '__main__':
    main()
