#!/usr/bin/env python3
"""Decode the game's njpeg images from the ROM, without running the game.

The game's full-screen dialogue backdrops are njpeg assets
(`docs/guides/njpeg-backgrounds.md`). An asset is

    u32 size | 'HU' | s8 qscale | u8 0 | u16 width | u16 height | 'HUFF' | s16 numMB | bits

The game decodes one in two halves:

* the CPU Huffman decode `func_8008B250` (main segment): per macroblock four Y
  blocks with the luminance tables (`0x800AA0A0` DC, `0x800AA120` AC), then two
  chroma blocks with the chrominance tables (`0x800AA0E0`, `0x800AA290`); the
  tables are the JPEG Annex K tables; bits are read MSB first with no byte
  stuffing; a block is 64 coefficients, EOB fills the rest with zeros;
* the RSP microcode `njpgdspMain` (`0x8009ED80`, data `0x800AC050`):
  dequantisation with the JPEG luminance table scaled by the task's qscale
  (N64 JPEG manual 5.3: -2, -1, 1, 2 are 1/4, 1/2, 1, 2), the inverse DCT,
  and 16-bit YUV out. The manual (5.1.5) says the DC terms are differences from
  the previous block and that the zigzag scan runs over the transposed block.

The game then draws the YUV macroblocks with S2DEX and the G_SETCONVERT matrix
`k0=175 k1=-43 k2=-89 k3=222` and copies the framebuffer back
(`func_ovlE_8019976C`). This tool does the same conversion in floating point,
so its output is 8-bit RGB where the game's readback is RGBA5551.

    tools/njpeg.py list  <rom>
    tools/njpeg.py decode <rom> -o <dir> [--rom-offset 0x...] [--yuv]
    tools/njpeg.py compare <rom> <rom offset> <reference.png> [--x X --y Y]

`compare` checks a decode against a capture of the port's own decode (for
example `tools/backgrounds.py extract 01`, the cathedral).
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import numpy as np

ROM_OF_VRAM = lambda v: v - 0x80070C60 + 0x1060  # main segment

HUFF_TABLES = {  # vram of the CPU decoder's tables: 16 u16 counts, then values
    "dc_lum": 0x800AA0A0,
    "dc_chr": 0x800AA0E0,
    "ac_lum": 0x800AA120,
    "ac_chr": 0x800AA290,
}
QTABLE_VRAM = 0x800AC070  # 64 u16, the JPEG luminance table, transposed

ZIGZAG = [  # JPEG zigzag: scan index -> (row, col)
    (0, 0), (0, 1), (1, 0), (2, 0), (1, 1), (0, 2), (0, 3), (1, 2),
    (2, 1), (3, 0), (4, 0), (3, 1), (2, 2), (1, 3), (0, 4), (0, 5),
    (1, 4), (2, 3), (3, 2), (4, 1), (5, 0), (6, 0), (5, 1), (4, 2),
    (3, 3), (2, 4), (1, 5), (0, 6), (0, 7), (1, 6), (2, 5), (3, 4),
    (4, 3), (5, 2), (6, 1), (7, 0), (7, 1), (6, 2), (5, 3), (4, 4),
    (3, 5), (2, 6), (1, 7), (2, 7), (3, 6), (4, 5), (5, 4), (6, 3),
    (7, 2), (7, 3), (6, 4), (5, 5), (4, 6), (3, 7), (4, 7), (5, 6),
    (6, 5), (7, 4), (7, 5), (6, 6), (5, 7), (6, 7), (7, 6), (7, 7),
]


KEY_BYTES = 64


def asset_key(rom: bytes, asset: dict) -> str:
    """The pack's name for an asset: FNV-1a 64 over the first 64 bytes of the
    compressed stream from its 'HUFF' tag (`njpeg_key` in
    app/src/hd_backgrounds.cpp, which reads the same bytes in RDRAM)."""
    h = 0xCBF29CE484222325
    for b in rom[asset["huff"]:asset["huff"] + KEY_BYTES]:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def load_huff(rom: bytes, vram: int) -> dict[tuple[int, int], int]:
    o = ROM_OF_VRAM(vram)
    counts = struct.unpack(">16H", rom[o:o + 32])
    n = sum(counts)
    values = struct.unpack(f">{n}H", rom[o + 32:o + 32 + 2 * n])
    table, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            table[(length, code)] = values[k]
            code += 1
            k += 1
        code <<= 1
    return table


class Bits:
    def __init__(self, data: bytes, pos: int = 0):
        self.data = data
        self.pos = pos  # bit position

    def bit(self) -> int:
        b = (self.data[self.pos >> 3] >> (7 - (self.pos & 7))) & 1
        self.pos += 1
        return b

    def bits(self, n: int) -> int:
        v = 0
        for _ in range(n):
            v = (v << 1) | self.bit()
        return v

    def huff(self, table) -> int:
        code = 0
        for length in range(1, 17):
            code = (code << 1) | self.bit()
            v = table.get((length, code))
            if v is not None:
                return v
        raise ValueError(f"bad Huffman code at bit {self.pos}")


def extend(v: int, size: int) -> int:
    if size == 0:
        return 0
    return v if v & (1 << (size - 1)) else v - (1 << size) + 1


def find_assets(rom: bytes) -> list[dict]:
    out = []
    o = rom.find(b"HUFF")
    while o != -1:
        if rom[o - 8:o - 6] == b"HU":
            w, h = struct.unpack(">HH", rom[o - 4:o])
            mb = struct.unpack(">h", rom[o + 4:o + 6])[0]
            if w and h and w % 16 == 0 and h % 16 == 0 and mb == (w // 16) * (h // 16):
                out.append({
                    "rom": o - 8,  # the 'HU' header
                    "qscale": struct.unpack(">b", rom[o - 6:o - 5])[0],
                    "w": w, "h": h, "mb": mb, "huff": o,
                })
        o = rom.find(b"HUFF", o + 1)
    return out


def huffman_decode(rom: bytes, asset: dict, tables: dict) -> np.ndarray:
    """The CPU half: coefficients per block, in stream (scan) order."""
    bits = Bits(rom, (asset["huff"] + 6) * 8)
    blocks = np.zeros((asset["mb"], 6, 64), dtype=np.int32)
    for m in range(asset["mb"]):
        for b in range(6):
            dc_t = tables["dc_lum"] if b < 4 else tables["dc_chr"]
            ac_t = tables["ac_lum"] if b < 4 else tables["ac_chr"]
            size = bits.huff(dc_t)
            blocks[m, b, 0] = extend(bits.bits(size), size)
            k = 1
            while k < 64:
                rs = bits.huff(ac_t)
                run, size = rs >> 4, rs & 15
                if run == 0 and size == 0:
                    break
                k += run
                if k >= 64:
                    raise ValueError(f"AC run past the block in macroblock {m}")
                blocks[m, b, k] = extend(bits.bits(size), size)
                k += 1
    return blocks


def idct_matrix() -> np.ndarray:
    c = np.zeros((8, 8))
    for k in range(8):
        for n in range(8):
            a = np.sqrt(1 / 8) if k == 0 else np.sqrt(2 / 8)
            c[k, n] = a * np.cos((2 * n + 1) * k * np.pi / 16)
    return c


def to_yuv(rom: bytes, asset: dict, blocks: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """The RSP half: Y (h x w), U and V (h/2 x w/2), as floats, U/V centred on 128."""
    o = ROM_OF_VRAM(QTABLE_VRAM)
    q = np.array(struct.unpack(">64H", rom[o:o + 128]), dtype=np.float64).reshape(8, 8)
    # The microcode keeps the table transposed and the scan runs over the
    # transposed block (manual 5.1.5), so both are transposed back here. An
    # untransposed scan doubled the error against the port's decode; a table
    # floored to integers after the 1/4 scale raised it from 1.2 to 2.9 levels.
    q = q.T * {-2: 0.25, -1: 0.5, 0: 1.0, 1: 1.0, 2: 2.0}[asset["qscale"]]

    # DC terms are differences from the previous block of the same component:
    # the four Y blocks chain one predictor, U and V their own.
    blocks = blocks.astype(np.float64)
    for b in (slice(0, 4), 4, 5):
        shape = blocks[:, b, 0].shape
        blocks[:, b, 0] = np.cumsum(blocks[:, b, 0].reshape(-1)).reshape(shape)

    nat = np.zeros(blocks.shape[:2] + (8, 8))
    for i, (r, c) in enumerate(ZIGZAG):
        nat[:, :, c, r] = blocks[:, :, i]
    nat *= q
    C = idct_matrix()
    pix = np.einsum("ki,mbkl,lj->mbij", C, nat, C)  # no level shift

    w, h = asset["w"], asset["h"]
    mw = w // 16
    Y = np.zeros((h, w))
    U = np.zeros((h // 2, w // 2))
    V = np.zeros((h // 2, w // 2))
    for m in range(blocks.shape[0]):
        my, mx = divmod(m, mw)
        y0, x0 = my * 16, mx * 16
        Y[y0:y0 + 8, x0:x0 + 8] = pix[m, 0]
        Y[y0:y0 + 8, x0 + 8:x0 + 16] = pix[m, 1]
        Y[y0 + 8:y0 + 16, x0:x0 + 8] = pix[m, 2]
        Y[y0 + 8:y0 + 16, x0 + 8:x0 + 16] = pix[m, 3]
        U[my * 8:my * 8 + 8, mx * 8:mx * 8 + 8] = pix[m, 4]
        V[my * 8:my * 8 + 8, mx * 8:mx * 8 + 8] = pix[m, 5]
    return Y, U, V


# G_SETCONVERT as the game sets it (func_ovlE_80199130: 0xEC15FD5D 0x3B78E42A):
# k0..k5 = 175, -43, -89, 222, 114, 42. The texture filter sign-extends k0..k3
# and scales them (2K+1)/256; the combiner, one cycle of
# (TEXEL0 - K4) * K5 + TEXEL0 (0xFC17FE2F 0x77FCF87C), applies k4 and k5 as
# x/255 (RT64 RasterPS.hlsl).
K0, K1, K2, K3, K4, K5 = 175, -43, -89, 222, 114, 42

# The RSP's output scale, measured against the port's own decode of the
# cathedral (tools/backgrounds.py extract 01): a least-squares fit on the
# 320x240 chunk 0x7CADC4 gave Y' = 1.0390*Y - 4.704 and U'/V' = 1.0347*(U/V-128).
# With these, the other three chunks (0x7D1B9C, 0x7D4EE4, 0x7D91D4) match the
# port's RGBA5551 exactly at 83-88 % of texels, mean error 1.0-1.4 levels of
# 255. The remainder is the microcode's fixed-point IDCT, which this float IDCT
# does not reproduce.
Y_SCALE, Y_OFFSET, C_SCALE = 1.0390, -4.704, 1.0347


def to_rgb(Y: np.ndarray, U: np.ndarray, V: np.ndarray) -> np.ndarray:
    """8-bit RGB as the port draws it, before the RGBA5551 framebuffer."""
    y = Y * Y_SCALE + Y_OFFSET
    u = (np.repeat(np.repeat(U, 2, axis=0), 2, axis=1) - 128.0) * C_SCALE
    v = (np.repeat(np.repeat(V, 2, axis=0), 2, axis=1) - 128.0) * C_SCALE
    k = lambda x: (2 * x + 1) / 256.0
    tex = np.stack([y + k(K0) * v, y + k(K1) * u + k(K2) * v, y + k(K3) * u], axis=-1)
    tex = np.clip(tex, 0, 255)
    out = tex + (tex - K4) * (K5 / 255.0)
    return np.clip(out, 0, 255)


def decode(rom: bytes, asset: dict) -> np.ndarray:
    tables = {k: load_huff(rom, v) for k, v in HUFF_TABLES.items()}
    blocks = huffman_decode(rom, asset, tables)
    return to_rgb(*to_yuv(rom, asset, blocks))


def find_compositions(rom: bytes, assets: list[dict]) -> list[dict]:
    """The `'B5'` containers: `'B5'`, u8 version (3), u8 count, s16 anchor x/y,
    then per chunk `s16 x, s16 y, u16 w, u16 h, u32 size, u32 0` and the chunk's
    njpeg asset (`size` bytes). `func_ovlE_8019A0A8` reads the chunk entries
    (`lhu 4/6` for the size, the asset at entry + 16). Every other asset is a
    single image. Each composition's `chunks` are (asset, x, y) in canvas
    pixels from the top-left, so a chunk's rectangle is (x, y, w, h)."""
    by_rom = {a["rom"]: a for a in assets}
    out = []
    o = rom.find(b"B5")
    while o != -1:
        count = rom[o + 3]
        p, chunks, ok = o + 8, [], 0 < count < 20
        for _ in range(count if ok else 0):
            x, y, w, h, size, _z = struct.unpack(">hhHHII", rom[p:p + 16])
            a = by_rom.get(p + 16)
            if a is None or (a["w"], a["h"]) != (w, h):
                ok = False
                break
            chunks.append((a, x, y))
            p += 16 + size
        if ok and chunks:
            x0 = min(x for _a, x, _y in chunks)
            y0 = min(y for _a, _x, y in chunks)
            chunks = [(a, x - x0, y - y0) for a, x, y in chunks]
            out.append({
                "rom": o,
                "chunks": chunks,
                "w": max(x + a["w"] for a, x, _y in chunks),
                "h": max(y + a["h"] for a, _x, y in chunks),
            })
        o = rom.find(b"B5", o + 1)
    return out


def images(rom: bytes) -> list[dict]:
    """Every picture to upscale: the compositions, then the single images.
    `name` is `b5-<rom>` or `s-<rom>`; `chunks` is (asset, x, y)."""
    assets = find_assets(rom)
    comps = find_compositions(rom, assets)
    used = {a["rom"] for c in comps for a, _x, _y in c["chunks"]}
    out = [{"name": f"b5-{c['rom']:07X}", **c} for c in comps]
    for a in assets:
        if a["rom"] not in used:
            out.append({"name": f"s-{a['rom']:07X}", "rom": a["rom"], "w": a["w"], "h": a["h"],
                        "chunks": [(a, 0, 0)]})
    return out


def render(rom: bytes, image: dict) -> np.ndarray:
    """The picture as 8-bit RGB floats; later chunks are drawn over earlier ones."""
    canvas = np.zeros((image["h"], image["w"], 3))
    for a, x, y in image["chunks"]:
        canvas[y:y + a["h"], x:x + a["w"]] = decode(rom, a)
    return canvas


def save_png(rgb: np.ndarray, path: Path) -> None:
    from PIL import Image
    Image.fromarray(np.round(rgb).astype(np.uint8), "RGB").save(path)


# --- HD pack ----------------------------------------------------------------
# The port writes `<pack>/njpeg/<key>.png` over the asset's pass
# (app/src/hd_backgrounds.cpp), so the texels the game later loads are known
# here, and RT64's hash of each strip can be computed without running the game
# (rt64_tmem_hasher.h, hash version 5; checked against all 116 strips of the
# cathedral capture). A capture supplies the draw: which rows each strip loads,
# and the bytes past the image that a strip reads (a 240-row chunk's last strip
# loads one row more: a heap block header and the next chunk's first texels).


def to5(c):
    return (c * 31 + 127) // 255  # to_5bit in app/src/hd_backgrounds.cpp


def expand5(v):
    return (v * 255 + 15) // 31  # an 8-bit value whose to5() is v


def rt64_hash(tmem: bytes, line: int, tmem_addr: int, width: int, height: int,
              siz: int = 2, fmt: int = 0, tlut: int = 0) -> str:
    """RT64's TMEMHasher::hash, version 5, for a load without a TLUT."""
    import xxhash
    draw_bpr = max((width << siz) >> 1, 1)
    tmem_bpr = line << 3
    addr = (tmem_addr << 3) & 4095
    x = xxhash.xxh3_64()

    def hash_tmem(base: int, count: int, odd: bool) -> None:
        if base + count > 4096:
            first = 4096 - base
            x.update(tmem[base:base + first])
            count, base = min(count - first, base), 0
        if odd:
            words = count // 8
            if words:
                x.update(tmem[base:base + words * 8])
                base += words * 8
                count -= words * 8
            if count > 4:
                x.update(tmem[base:base + count - 4])
                x.update(tmem[base + 4:base + 8])
            elif count > 0:
                x.update(tmem[base + 4:base + 4 + count])
        else:
            x.update(tmem[base:base + count])

    if tmem_bpr > draw_bpr:
        for i in range(height):
            hash_tmem((addr + i * tmem_bpr) & 4095, draw_bpr, bool(i & 1))
    else:
        hash_tmem(addr, tmem_bpr * (height - 1) + draw_bpr, False)
    x.update(struct.pack("<HHIHBB", width, height, tlut, line, siz, fmt))
    return x.hexdigest()


def load_tile_tmem(rows: list[bytes], line: int, tmem_addr: int) -> bytes:
    """TMEM after a G_LOADTILE of these rows (guest byte order): row i at
    `tmem_addr*8 + i*line*8`, odd rows with the 32-bit words of each 64-bit
    word swapped."""
    tmem = bytearray(4096)
    for i, row in enumerate(rows):
        row = bytes(row).ljust(line * 8, b"\0")[:line * 8]
        if i & 1:
            row = b"".join(row[k + 4:k + 8] + row[k:k + 4] for k in range(0, len(row), 8))
        base = (tmem_addr * 8 + i * line * 8) & 4095
        for k, b in enumerate(row):
            tmem[(base + k) & 4095] = b
    return bytes(tmem)


def read_capture(dirs: list[Path]) -> dict:
    """The RGBA16 tile loads of RT64 texture dumps (`OGRE_DUMP_TEX`), grouped
    by (image address, row width): {(addr, w): {"strips": {ult: ...},
    "mem": {address: byte}}}; `mem` holds every byte the loads read."""
    import json
    groups: dict = {}
    for d in dirs:
        for f in Path(d).glob("*.v5.rice.json"):
            j = json.loads(f.read_text())
            tex, tile = j.get("texture", {}), j.get("tile", {})
            if j.get("type") != "Tile" or tex.get("fmt") != 0 or tex.get("siz") != 2:
                continue
            t = json.loads(f.with_name(f.name.replace(".rice.json", ".tile.json")).read_text())
            w, h = t["width"], t["height"]
            if tile.get("uls", 0) != 0 or w != tex.get("width"):
                continue
            rd = f.with_name(f.name.replace(".rice.json", ".rice.rdram")).read_bytes()
            if len(rd) < w * h * 2:
                continue
            guest = bytes(rd[i ^ 3] for i in range(w * h * 2))
            ult = tile["ult"] >> 2
            addr = 0x80000000 | tex["address"]
            g = groups.setdefault((addr, w), {"strips": {}, "mem": {}})
            g["strips"][ult] = {"ult": ult, "rows": h, "line": tile["line"],
                                "tmem": tile["tmem"], "hash": f.name.split(".")[0]}
            base = addr + ult * w * 2
            for i, b in enumerate(guest):
                g["mem"][base + i] = b
    return groups


def px5_of(rgb: np.ndarray) -> np.ndarray:
    """8-bit RGB -> guest RGBA5551 values, as the port converts the PNG."""
    q = to5(np.round(rgb).astype(np.int64))
    return (q[..., 0] << 11) | (q[..., 1] << 6) | (q[..., 2] << 1) | 1


def match_group(groups: dict, w: int, h: int, ref5: np.ndarray):
    """The captured image that holds this chunk's texels, within the decoder's
    error: the mean 5-bit difference over the sampled rows must be below 1."""
    best = None
    for (addr, gw), g in groups.items():
        if gw != w:
            continue
        rows = {r for st in g["strips"].values() for r in range(st["ult"], st["ult"] + st["rows"])
                if r < h}
        if len(rows) < h * 0.9:
            continue
        diffs = []
        for r in sorted(rows)[::7]:
            row = bytes(g["mem"].get(addr + r * w * 2 + k, 0) for k in range(w * 2))
            got = np.frombuffer(row, dtype=">u2").astype(np.int64)
            for sh in (11, 6, 1):
                diffs.append(np.abs(((got >> sh) & 31) - ((ref5[r] >> sh) & 31)).mean())
        score = float(np.mean(diffs))
        if score < 1.0 and (best is None or score < best[0]):
            best = (score, addr, g)
    return best


def cmd_pack(a) -> int:
    import json
    import shutil
    from PIL import Image
    rom = Path(a.rom).read_bytes()
    layout = json.loads(Path(a.layout).read_text())
    W, H, S = layout["w"], layout["h"], a.scale
    master = Image.open(a.master).convert("RGB")
    hd = np.asarray(master.resize((W * S, H * S), Image.LANCZOS))
    canvas5 = px5_of(np.asarray(master.resize((W, H), Image.BOX), dtype=np.float64))
    by_rom = {f"0x{x['rom']:07X}": x for x in find_assets(rom)}
    groups = read_capture([Path(d) for d in a.capture])
    pack = Path(a.pack)
    folder = a.folder
    out_hd = pack / "hd" / folder
    if out_hd.is_dir():
        shutil.rmtree(out_hd)
    out_hd.mkdir(parents=True)
    (pack / "njpeg").mkdir(parents=True, exist_ok=True)
    db_path = pack / "hd" / "rt64.json"
    db = json.loads(db_path.read_text()) if db_path.is_file() else {
        "configuration": {"autoPath": "rt64", "configurationVersion": 3,
                          "defaultOperation": "stall", "defaultShift": "half",
                          "hashVersion": 5}, "textures": []}
    textures = [t for t in db["textures"] if not t["path"].startswith(f"{folder}/")]
    taken = {t["hashes"]["rt64"]: t["path"] for t in textures}

    # Match each chunk to its captured image, so that every captured address a
    # strip reads can be given the pack's texels (`overlay`).
    chunks = []
    for c in layout["chunks"]:
        x, y, w, h = c["x"], c["y"], c["w"], c["h"]
        m = match_group(groups, w, h, px5_of(decode(rom, by_rom[c["rom"]]))) if groups else None
        if m is None and a.assume:
            # The draw seen in every capture so far: strips of 2048 // w rows
            # (as many as fit TMEM's 4 KiB), each starting one row before the
            # previous one ends, from line 0, TMEM 0. Only strips inside the
            # chunk are computed; the last one is assumed clamped to the
            # chunk. A strip that reads past the chunk needs a capture.
            rows = 2048 // w
            sts, ult = [], 0
            while ult < h - 1:
                sts.append({"ult": ult, "rows": min(rows, h - ult), "line": (w * 2 + 7) // 8, "tmem": 0})
                ult += rows - 1
            m = (float("nan"), None, {"strips": {s["ult"]: s for s in sts}, "mem": {}})
            print(f"njpeg.py: {layout['name']} chunk {c['rom']} {w}x{h}: not in the capture; "
                  f"assuming {len(sts)} strips of {rows} rows", file=sys.stderr)
        elif m is None:
            print(f"njpeg.py: {layout['name']} chunk {c['rom']} {w}x{h}: not in the capture; "
                  f"no HD strips for it", file=sys.stderr)
        chunks.append({**c, "px5": canvas5[y:y + h, x:x + w].copy(), "match": m})

    def overlay(addr: int, n: int, mem: dict) -> bytes:
        out = bytearray(n)
        for i in range(n):
            p = addr + i
            for ch in chunks:
                if ch["match"] is None or ch["match"][1] is None:
                    continue
                off = p - ch["match"][1]
                if 0 <= off < ch["w"] * ch["h"] * 2:
                    v = int(ch["px5"].reshape(-1)[off // 2])
                    out[i] = (v >> 8) & 0xFF if off % 2 == 0 else v & 0xFF
                    break
            else:
                out[i] = mem.get(p, 0)
        return bytes(out)

    written, seen = 0, set()
    for ch in chunks:
        if ch["match"] is None:
            continue
        score, base, g = ch["match"]
        w, h = ch["w"], ch["h"]
        strips = sorted(g["strips"].values(), key=lambda s: s["ult"])
        def strip_rows(st):
            if base is None:  # assumed draw: the rows are the chunk's own
                return [ch["px5"][st["ult"] + i].astype(">u2").tobytes() for i in range(st["rows"])]
            return [overlay(base + (st["ult"] + i) * w * 2, w * 2, g["mem"]) for i in range(st["rows"])]

        for k, st in enumerate(strips):
            # The row a mark may change: one no neighbouring strip loads (the
            # first row is the previous strip's last, the last row the next
            # strip's first), inside the chunk.
            last = k == len(strips) - 1
            mark_rows = [r for r in (st["ult"] + 2, st["ult"] + 1, st["ult"] + st["rows"] - 1)
                         if st["ult"] < r < h and (r < st["ult"] + st["rows"] - 1 or last)]
            skip = False
            for attempt in range(64):
                rows = strip_rows(st)
                hsh = rt64_hash(load_tile_tmem(rows, st["line"], st["tmem"]),
                                st["line"], st["tmem"], w, st["rows"])
                own = ch["px5"][st["ult"]:min(h, st["ult"] + st["rows"])]
                flat = own.size > 0 and bool((own == own.flat[0]).all())
                if hsh not in seen and hsh not in taken and not flat:
                    break
                # A strip that repeats another, or is one flat colour (which any
                # flat game texture of its size shares), gets the lowest blue bit
                # of one texel flipped in its third row, which no neighbour
                # loads. The changed texel goes into njpeg/<key>.png too.
                if not mark_rows:
                    skip = True
                    break
                ch["px5"][mark_rows[0], attempt % w] ^= 2
            if skip:
                print(f"njpeg.py: {layout['name']} chunk {ch['rom']} strip at row {st['ult']} "
                      f"repeats another texture and has no row to mark; left at game size",
                      file=sys.stderr)
                continue
            seen.add(hsh)
            y0 = (ch["y"] + st["ult"]) * S
            crop = hd[y0:, ch["x"] * S:(ch["x"] + w) * S]
            need = st["rows"] * S
            if crop.shape[0] < need:
                crop = np.concatenate([crop, np.repeat(crop[-1:], need - crop.shape[0], axis=0)])
            Image.fromarray(np.ascontiguousarray(crop[:need])).save(out_hd / f"{hsh}.png")
            textures.append({"path": f"{folder}/{hsh}.png", "hashes": {"rt64": hsh, "rice": ""}})
            taken[hsh] = f"{folder}/{hsh}.png"
            written += 1
        where = "assumed draw" if base is None else f"at 0x{base:08X} (capture match {score:.2f})"
        print(f"njpeg.py: {layout['name']} chunk {ch['rom']} {w}x{h} {where}: {len(strips)} strips")
    for ch in chunks:
        q = ch["px5"]
        rgb = np.stack([expand5((q >> sh) & 31) for sh in (11, 6, 1)], axis=-1).astype(np.uint8)
        Image.fromarray(rgb, "RGB").save(pack / "njpeg" / f"{ch['key']}.png")
    db["textures"] = textures
    db_path.write_text(json.dumps(db, indent=1) + "\n")
    if a.src:
        src = Path(a.src)
        src.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(a.master, src / f"{layout['name']}-master.png")
        (src / f"{layout['name']}.json").write_text(json.dumps(layout, indent=1))
    print(f"njpeg.py: {out_hd}: {written} strip(s) at {S}x; {db_path}: {len(textures)} texture(s)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("rom")
    p = sub.add_parser("decode")
    p.add_argument("rom")
    p.add_argument("-o", "--out", required=True)
    p.add_argument("--rom-offset", type=lambda s: int(s, 0))
    p = sub.add_parser("prep")
    p.add_argument("rom")
    p.add_argument("-o", "--out", required=True)
    p.add_argument("--only", help="comma-separated image names (b5-..., s-...)")
    p.add_argument("--factor", type=int, default=4)
    p = sub.add_parser("pack")
    p.add_argument("rom")
    p.add_argument("layout", help="<name>.json from prep")
    p.add_argument("master", help="the HD art for the whole picture")
    p.add_argument("--capture", action="append", default=[],
                   help="an OGRE_DUMP_TEX folder from a run that drew this picture's pipeline")
    p.add_argument("--assume", action="store_true",
                   help="for a chunk no capture holds, assume the measured strip draw")
    p.add_argument("--pack", default=str(Path(__file__).resolve().parent.parent / "mods" / "backgrounds"))
    p.add_argument("--folder", required=True)
    p.add_argument("--scale", type=int, default=6)
    p.add_argument("--src", help="keep the master and layout here (hd-src)")
    p = sub.add_parser("compare")
    p.add_argument("rom")
    p.add_argument("offset", type=lambda s: int(s, 0))
    p.add_argument("reference")
    p.add_argument("--x", type=int, default=0)
    p.add_argument("--y", type=int, default=0)
    a = ap.parse_args()

    rom = Path(a.rom).read_bytes()
    assets = find_assets(rom)
    if a.cmd == "list":
        keys = set()
        for x in assets:
            key = asset_key(rom, x)
            keys.add(key)
            print(f"0x{x['rom']:07X}  {x['w']}x{x['h']}  mb={x['mb']}  q={x['qscale']}  key={key}")
        print(len(assets), "assets,", len(keys), "keys")
        return 0
    if a.cmd == "decode":
        out = Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        for x in assets:
            if a.rom_offset is not None and x["rom"] != a.rom_offset:
                continue
            path = out / f"njpeg-{x['rom']:07X}-{x['w']}x{x['h']}.png"
            save_png(decode(rom, x), path)
            print(path)
        return 0
    if a.cmd == "pack":
        return cmd_pack(a)
    if a.cmd == "prep":
        # The upscaler input: the decoded picture enlarged `factor` times with
        # Lanczos, plus `<name>.json` with each chunk's asset key and rectangle,
        # which `pack` cuts the HD master by.
        import json
        from PIL import Image
        out = Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        only = set(a.only.split(",")) if a.only else None
        for im in images(rom):
            if only is not None and im["name"] not in only:
                continue
            rgb = render(rom, im)
            save_png(rgb, out / f"{im['name']}-orig.png")
            big = Image.fromarray(np.round(rgb).astype(np.uint8), "RGB").resize(
                (im["w"] * a.factor, im["h"] * a.factor), Image.LANCZOS)
            big.save(out / f"{im['name']}.png")
            layout = {"name": im["name"], "w": im["w"], "h": im["h"], "factor": a.factor,
                      "chunks": [{"key": asset_key(rom, c), "rom": f"0x{c['rom']:07X}",
                                  "x": x, "y": y, "w": c["w"], "h": c["h"]}
                                 for c, x, y in im["chunks"]]}
            (out / f"{im['name']}.json").write_text(json.dumps(layout, indent=1))
            print(f"{out / im['name']}.png  {im['w']}x{im['h']} x{a.factor}  "
                  f"{len(im['chunks'])} chunk(s)")
        return 0
    if a.cmd == "compare":
        from PIL import Image
        x = next(t for t in assets if t["rom"] == a.offset)
        rgb = decode(rom, x)
        ref = np.asarray(Image.open(a.reference).convert("RGB"), dtype=np.float64)
        ref = ref[a.y:a.y + x["h"], a.x:a.x + x["w"]]
        hh, ww = ref.shape[:2]
        # The reference is the RGBA5551 readback, expanded as OGRE_BG_DUMP
        # does (v * 255 / 31, truncated); quantise ours the same way.
        ours = np.floor(np.round(rgb[:hh, :ww] * 31 / 255) * 255 / 31)
        d = np.abs(ours - ref)
        print(f"mean |diff| {d.mean():.2f}  max {d.max():.0f}  "
              f"exact {np.mean(d == 0) * 100:.1f}%  "
              f"means ours {ours.reshape(-1, 3).mean(0).round(1)} ref {ref.reshape(-1, 3).mean(0).round(1)}")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
