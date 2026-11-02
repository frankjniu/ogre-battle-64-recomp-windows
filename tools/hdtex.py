#!/usr/bin/env python3
"""Inventory, upscale and pack the game's textures as RT64 HD replacements.

RT64 replaces a texture by the hash of what a load put in TMEM
(`docs/guides/hd-workflow.md` section 7). A run with `OGRE_DUMP_TEX=<dir>`
writes one set of files per hash. This tool reads such a dump:

    tools/hdtex.py scan <dump> -o <inv>
        decode every dumped texture to <inv>/png/<hash>.png, write
        <inv>/index.json (hash, load, format, size, address, palette) and
        contact sheets <inv>/sheet-<n>.png labelled with each entry's number.

    tools/hdtex.py merge <inv> <inv> ... -o <inv>
        one inventory from several scans (WIDESCREEN off and on, other
        routes), so a source image keeps every piece any run loaded.

    tools/hdtex.py prep <inv> <key>[,<key>...] -o <in.png> [--tile <keys>]
        one image for the AI upscaler holding the named source images
        (images.json keys), with alpha handled and wrapping textures laid
        out as 3x3 periods, plus <in.json> with the layout.

    tools/hdtex.py pack <in.json> <upscaled.png> --folder <name>
        cut every piece (hash) of those source images out of the upscaled
        image at an integer scale (at most 8x), put the alpha back, and write
        mods/backgrounds/hd/<name>/<hash>.png and the hd/rt64.json entries.
        Each source image's HD master is kept in mods/backgrounds/hd-src/.

    tools/hdtex.py repack <inv> --folder <name>
        cut the masters in hd-src/<name> again for a new scan: a changed draw
        or another route loads other pieces (other hashes) of the same image.
        A source image matches a master by format, row width and equal texels
        where both are opaque, not by address, since heap images move.

The scan groups tile loads from one RDRAM image and palette into a source
image (<inv>/images/<key>.png), placing each load at its (uls, ult); a block
load is its own image. An HD version of the source image then yields every
piece, the way `tools/backgrounds.py hd` cuts backdrop strips.

Decoding reads the `.rice.rdram` range RT64 dumped for the load that filled
TMEM (`rt64_rdp_tmem.cpp`, the `.rice.rdram` writer): a tile load's texel
(0, 0) is at the dump's start and its rows are the image's row stride; a block
load's rows are the render tile's `line` (64-bit words). The dump is host
RDRAM, so the guest byte at address A is host byte A ^ 3. A CI texture's
palette is the `.rice.palette.rdram` range, RGBA16 or IA16 per the tile's
TLUT mode.

Needs Pillow and numpy.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HD = ROOT / "mods" / "backgrounds" / "hd"
# The HD master of each source image, kept so a changed draw or a new route
# needs a new scan and `repack`, not a new upscale. Derived from the ROM, so
# gitignored like hd/.
HD_SRC = ROOT / "mods" / "backgrounds" / "hd-src"

FMT_NAMES = {0: "RGBA", 1: "YUV", 2: "CI", 3: "IA", 4: "I"}
SIZ_BITS = {0: 4, 1: 8, 2: 16, 3: 32}


def libs():
    try:
        import numpy as np
        from PIL import Image, ImageDraw
    except ImportError:
        raise SystemExit("hdtex.py: needs Pillow and numpy (pip install pillow numpy)")
    return np, Image, ImageDraw


# --- decoding --------------------------------------------------------------

class GuestBytes:
    """A host-order RDRAM range as RT64 dumped it: `data` holds host bytes
    `start .. start + len(data)`. The guest byte at address A is host A ^ 3."""

    def __init__(self, data: bytes, start: int):
        self.data = data
        self.start = start

    def byte(self, addr: int) -> int:
        i = (addr ^ 3) - self.start
        if 0 <= i < len(self.data):
            return self.data[i]
        return 0

    def u16(self, addr: int) -> int:
        return (self.byte(addr) << 8) | self.byte(addr + 1)


def rice_range(load: dict, render_tile: dict, width: int, height: int) -> int:
    """The host start address of RT64's `.rice.rdram` range for a load
    (the same arithmetic as the writer), and the guest address of texel
    (0, 0)."""
    tex, lt = load["texture"], load["tile"]
    siz = tex["siz"]
    offset = (lt["uls"] >> 2) << siz >> 1
    bpr = tex["width"] << siz >> 1
    if load["type"] == "Block":
        return tex["address"] + offset + bpr * lt["ult"]
    return tex["address"] + offset + bpr * (lt["ult"] >> 2)


def rgba16(v: int) -> tuple[int, int, int, int]:
    r, g, b = (v >> 11) & 31, (v >> 6) & 31, (v >> 1) & 31
    return ((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255 if v & 1 else 0)


def ia16(v: int) -> tuple[int, int, int, int]:
    i, a = v >> 8, v & 0xFF
    return (i, i, i, a)


def decode(entry: dict, dump: Path):
    """The texture's texels as an RGBA numpy array (height, width, 4), or None
    when the dump lacks what the decode needs."""
    np, _, _ = libs()
    base = dump / entry["base"]
    load = json.loads((dump / f"{entry['base']}.rice.json").read_text())
    tile = json.loads((dump / f"{entry['base']}.tile.json").read_text())
    rt = tile["tile"]
    w, h = tile["width"], tile["height"]
    fmt, siz = rt["fmt"], rt["siz"]
    rdram_path = Path(str(base) + ".rice.rdram")
    tmem_only = entry.get("tmem_only", False)
    if tmem_only:
        # No RDRAM load is on record for this TMEM address (a render tile
        # that samples part of what another tile's load put there): read
        # the texels from the TMEM snapshot, the bytes the hash covers.
        # RT64 keeps TMEM in N64 byte order; odd rows have their 32-bit
        # words swapped (`ogreDecodeTMEM` in rt64_rdp_tmem.cpp).
        if siz == 3 or not Path(str(base) + ".tmem").is_file():
            return None
        tmem = Path(str(base) + ".tmem").read_bytes()
        line = rt["line"] << 3

        class TmemBytes:
            def byte(self, addr: int) -> int:
                rel = addr - (rt["tmem"] << 3)
                row, col = divmod(rel, line) if line else (0, rel)
                if row & 1:
                    col ^= 4
                return tmem[((rt["tmem"] << 3) + row * line + col) & 0xFFF]

            def u16(self, addr: int) -> int:
                return (self.byte(addr) << 8) | self.byte(addr + 1)

        mem = TmemBytes()
        start = rt["tmem"] << 3
    else:
        if not rdram_path.is_file():
            return None
        start = rice_range(load, rt, w, h)
        mem = GuestBytes(rdram_path.read_bytes(), start)
    lsiz = load["texture"]["siz"]
    if tmem_only:
        stride, first = rt["line"] << 3, start
    elif load["type"] == "Block":
        stride = rt["line"] * 8 * (2 if siz == 3 else 1)
    else:
        stride = load["texture"]["width"] << lsiz >> 1
    if not tmem_only:
        # A render tile whose TMEM address differs from the load's starts
        # further into the loaded data.
        first = start + (rt["tmem"] - load["tile"]["tmem"]) * 8 if load["type"] == "Block" else start

    palette = None
    if fmt == 2:
        pjson = dump / f"{entry['base']}.rice.palette.json"
        prd = dump / f"{entry['base']}.rice.palette.rdram"
        if not (pjson.is_file() and prd.is_file()):
            return None
        pload = json.loads(pjson.read_text())
        ptex, ptile = pload["texture"], pload["tile"]
        pstart = (ptex["address"] + ((ptile["uls"] >> 2) << ptex["siz"] >> 1)
                  + (ptex["width"] << ptex["siz"] >> 1) * (ptile["ult"] >> 2))
        pmem = GuestBytes(prd.read_bytes(), pstart)
        tmem_pal = 256 + (rt["palette"] << 4 if siz == 0 else 0)
        skip = tmem_pal - ptile["tmem"]
        conv = ia16 if tile.get("tlut") == "IA16" else rgba16
        palette = [conv(pmem.u16(pstart + 2 * (skip + i))) for i in range(256)]

    out = np.zeros((h, w, 4), dtype=np.uint8)
    for y in range(h):
        row = first + y * stride
        for x in range(w):
            if siz == 0:
                b = mem.byte(row + (x >> 1))
                v = (b >> 4) if (x & 1) == 0 else (b & 15)
            elif siz == 1:
                v = mem.byte(row + x)
            elif siz == 2:
                v = mem.u16(row + 2 * x)
            else:
                a = row + 4 * x
                out[y, x] = (mem.byte(a), mem.byte(a + 1), mem.byte(a + 2), mem.byte(a + 3))
                continue
            if fmt == 2:
                out[y, x] = palette[v] if siz == 1 else palette[v & 15]
            elif fmt == 0:
                out[y, x] = rgba16(v)
            elif fmt == 3:
                if siz == 0:
                    i = ((v >> 1) & 7) * 255 // 7
                    out[y, x] = (i, i, i, 255 if v & 1 else 0)
                elif siz == 1:
                    i, a = (v >> 4) * 17, (v & 15) * 17
                    out[y, x] = (i, i, i, a)
                else:
                    out[y, x] = ia16(v)
            elif fmt == 4:
                i = v * 17 if siz == 0 else v
                out[y, x] = (i, i, i, 255)
            else:
                return None
    return out


def read_dump(dump: Path) -> list[dict]:
    entries = []
    for rice in sorted(dump.glob("*.rice.json")):
        base = rice.name[: -len(".rice.json")]
        if base.endswith(".rice.palette"):
            continue
        tile_json = dump / f"{base}.tile.json"
        if not tile_json.is_file():
            continue
        load = json.loads(rice.read_text())
        tile = json.loads(tile_json.read_text())
        rt = tile["tile"]
        hash_hex, _, ver = base.partition(".v")
        e = {
            "hash": hash_hex,
            "version": int(ver or 0),
            "base": base,
            "type": load.get("type"),
            "address": 0x80000000 | load["texture"]["address"],
            "image_width": load["texture"]["width"],
            "uls": load["tile"]["uls"] >> 2,
            "ult": load["tile"]["ult"] >> 2 if load.get("type") == "Tile" else load["tile"]["ult"],
            "fmt": FMT_NAMES.get(rt["fmt"], str(rt["fmt"])),
            "bits": SIZ_BITS.get(rt["siz"], 0),
            "width": tile["width"],
            "height": tile["height"],
            "tlut": tile.get("tlut"),
            "palette": rt["palette"],
            "cms": rt["cms"], "cmt": rt["cmt"], "masks": rt["masks"], "maskt": rt["maskt"],
            "tmem_only": load["texture"]["address"] == 0 and load["texture"]["width"] == 0,
        }
        pj = dump / f"{base}.rice.palette.json"
        if pj.is_file():
            e["palette_address"] = 0x80000000 | json.loads(pj.read_text())["texture"]["address"]
        entries.append(e)
    return entries


# --- scan ------------------------------------------------------------------

def cmd_scan(args) -> int:
    np, Image, ImageDraw = libs()
    dump = Path(args.dump)
    out = Path(args.out)
    (out / "png").mkdir(parents=True, exist_ok=True)
    entries = read_dump(dump)
    # Order: by source address, then by row, so a strip set reads in order.
    entries.sort(key=lambda e: (e["address"], e["ult"], e["uls"], e["hash"]))
    decoded = 0
    for n, e in enumerate(entries):
        e["n"] = n
        try:
            px = decode(e, dump)
        except Exception as exc:  # a malformed dump entry; keep going
            e["error"] = str(exc)
            px = None
        if px is None:
            e["decoded"] = False
            continue
        e["decoded"] = True
        a = px[:, :, 3]
        e["alpha"] = "opaque" if a.min() == 255 else "binary" if set(np.unique(a)) <= {0, 255} else "blend"
        e["mean"] = [round(float(px[:, :, c][a > 0].mean()), 1) if (a > 0).any() else 0 for c in range(3)]
        Image.fromarray(px, "RGBA").save(out / "png" / f"{e['hash']}.png")
        decoded += 1
    (out / "index.json").write_text(json.dumps(entries, indent=1))
    print(f"hdtex.py: {len(entries)} textures, {decoded} decoded -> {out}")

    # Contact sheets: each texture at 2x on a checkerboard, labelled.
    cell_w, cell_h, cols = 280, 150, 8
    per_sheet = cols * 10
    shown = [e for e in entries if e.get("decoded")]
    for s in range(0, len(shown), per_sheet):
        chunk = shown[s:s + per_sheet]
        rows = math.ceil(len(chunk) / cols)
        sheet = Image.new("RGB", (cols * cell_w, rows * cell_h), (40, 40, 48))
        draw = ImageDraw.Draw(sheet)
        for i, e in enumerate(chunk):
            x0, y0 = (i % cols) * cell_w, (i // cols) * cell_h
            img = Image.open(out / "png" / f"{e['hash']}.png")
            scale = max(1, min(4, (cell_w - 8) // max(1, img.width), (cell_h - 24) // max(1, img.height)))
            big = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
            if big.width > cell_w - 8 or big.height > cell_h - 24:
                big.thumbnail((cell_w - 8, cell_h - 24))
            bg = Image.new("RGBA", big.size, (90, 90, 100, 255))
            bg.alpha_composite(big)
            sheet.paste(bg.convert("RGB"), (x0 + 4, y0 + 20))
            label = f"{e['n']} {e['fmt']}{e['bits']} {e['width']}x{e['height']} {e['address']:08X}"
            draw.text((x0 + 4, y0 + 4), label, fill=(230, 230, 230))
        sheet.save(out / f"sheet-{s // per_sheet:02d}.png")
    print(f"hdtex.py: {math.ceil(len(shown) / per_sheet)} contact sheet(s)")

    groups = assemble(entries, out)
    (out / "images.json").write_text(json.dumps(groups, indent=1))
    print(f"hdtex.py: {len(groups)} source images -> {out / 'images'}")
    return 0


def group_key(e: dict) -> str:
    """Tile loads from one RDRAM image with one palette are pieces of one
    picture. A block load is its own picture."""
    if e.get("tmem_only"):
        return f"tmem-{e['fmt']}{e['bits']}-{e['hash']}"
    if e["type"] != "Tile":
        return f"{e['address']:08X}-{e['fmt']}{e['bits']}-block-{e['hash']}"
    pal = e.get("palette_address", 0)
    return f"{e['address']:08X}-{e['fmt']}{e['bits']}-w{e['image_width']}-p{pal:08X}"


def assemble(entries: list[dict], out: Path) -> list[dict]:
    """Place every decoded tile load at its (uls, ult) in its source image and
    write one PNG per source image. Returns the groups with their pieces."""
    np, Image, _ = libs()
    (out / "images").mkdir(exist_ok=True)
    by_key: dict[str, list[dict]] = {}
    for e in entries:
        if e.get("decoded"):
            by_key.setdefault(group_key(e), []).append(e)
    groups = []
    for key, pieces in sorted(by_key.items()):
        own = pieces[0]["type"] != "Tile" or pieces[0].get("tmem_only")
        x0 = 0 if own else min(p["uls"] for p in pieces)
        y0 = 0 if own else min(p["ult"] for p in pieces)
        if own:
            for p in pieces:
                p["uls"] = p["ult"] = 0
        w = max(p["uls"] + p["width"] for p in pieces) - x0
        h = max(p["ult"] + p["height"] for p in pieces) - y0
        canvas = np.zeros((h, w, 4), np.uint8)
        conflicts = 0
        for p in sorted(pieces, key=lambda p: (p["ult"], p["uls"])):
            px = np.asarray(Image.open(out / "png" / f"{p['hash']}.png").convert("RGBA"))
            ys, xs = p["ult"] - y0, p["uls"] - x0
            region = canvas[ys:ys + p["height"], xs:xs + p["width"]]
            filled = region[:, :, 3] > 0
            if filled.any() and (region[filled] != px[: region.shape[0], : region.shape[1]][filled]).any():
                conflicts += 1
            canvas[ys:ys + p["height"], xs:xs + p["width"]] = px[: region.shape[0], : region.shape[1]]
        Image.fromarray(canvas, "RGBA").save(out / "images" / f"{key}.png")
        groups.append({"key": key, "x0": x0, "y0": y0, "width": w, "height": h,
                       "pieces": len(pieces), "conflicts": conflicts,
                       "fmt": f"{pieces[0]['fmt']}{pieces[0]['bits']}",
                       "image_width": pieces[0]["image_width"] if pieces[0]["type"] == "Tile" else None,
                       "hashes": [p["hash"] for p in pieces]})
    return groups


# --- atlas / cut -------------------------------------------------------------

def alpha_kind(np, alpha) -> str:
    if alpha.min() == 255:
        return "opaque"
    if set(np.unique(alpha).tolist()) <= {0, 255}:
        return "binary"
    return "soft"


def upscaler_rgb(np, px):
    """The colour an upscaler should see for an RGBA image.

    opaque: unchanged. binary alpha: each transparent texel within 2 texels
    of an opaque one takes the mean of its opaque neighbours, so the edge
    colour continues and the cut-out edge has no fringe; the rest is black.
    soft alpha: premultiplied over black, which `pack` divides back out."""
    rgb = px[:, :, :3].astype(np.float32)
    alpha = px[:, :, 3]
    kind = alpha_kind(np, alpha)
    if kind == "opaque":
        return px[:, :, :3].copy()
    if kind == "soft":
        return (rgb * (alpha[:, :, None] / 255.0)).round().clip(0, 255).astype(np.uint8)
    known = alpha > 0
    rgb[~known] = 0
    for _ in range(2):
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            k = np.roll(known, (dy, dx), (0, 1))
            acc += np.roll(rgb, (dy, dx), (0, 1)) * k[:, :, None]
            cnt += k
        new = (~known) & (cnt > 0)
        rgb[new] = acc[new] / cnt[new][:, None]
        known = known | new
    return rgb.clip(0, 255).astype(np.uint8)


def hd_alpha(np, Image, alpha, scale: int):
    """The original alpha at `scale` times: bicubic, and binary alpha
    thresholded at half so edges stay hard but follow the shape."""
    if alpha.min() == 255:
        return np.full((alpha.shape[0] * scale, alpha.shape[1] * scale), 255, np.uint8)
    a = np.asarray(Image.fromarray(alpha, "L").resize(
        (alpha.shape[1] * scale, alpha.shape[0] * scale), Image.BICUBIC))
    if alpha_kind(np, alpha) == "binary":
        a = np.where(a >= 128, 255, 0).astype(np.uint8)
    return a


def merge_pack(folder: str, hashes: list[str]) -> list[str]:
    """Add hd/<folder>/<hash>.png entries to hd/rt64.json. A hash that
    another folder already owns (a backdrop strip, say) stays with it.
    Returns the hashes added."""
    db_path = HD / "rt64.json"
    if db_path.is_file():
        db = json.loads(db_path.read_text())
    else:
        # The same configuration `tools/backgrounds.py` writes.
        db = {"configuration": {"autoPath": "rt64", "configurationVersion": 3,
                                "defaultOperation": "stall", "defaultShift": "half",
                                "hashVersion": 5}, "textures": []}
    taken = {t["hashes"]["rt64"]: t["path"] for t in db.get("textures", [])
             if not t.get("path", "").startswith(f"{folder}/")}
    for h in [h for h in hashes if h in taken]:
        print(f"hdtex.py: hash {h} is already {taken[h]}; not replacing it", file=sys.stderr)
        (HD / folder / f"{h}.png").unlink(missing_ok=True)
    hashes = [h for h in hashes if h not in taken]
    keep = [t for t in db.get("textures", []) if t.get("hashes", {}).get("rt64") not in set(hashes)]
    for h in hashes:
        keep.append({"path": f"{folder}/{h}.png", "hashes": {"rt64": h, "rice": ""}})
    db["textures"] = keep
    db_path.write_text(json.dumps(db, indent=1) + "\n")
    return hashes


def find_group(inv: Path, key: str) -> dict:
    groups = json.loads((inv / "images.json").read_text())
    hits = [g for g in groups if g["key"] == key]
    if not hits:
        hits = [g for g in groups if g["key"].startswith(key)]
    if len(hits) != 1:
        raise SystemExit(f"hdtex.py: '{key}' names {len(hits)} source images in {inv}")
    return hits[0]


def cmd_prep(args) -> int:
    """One upscaler input for one or more source images.

    Each image is placed with a gutter of repeated edge texels; a `--tile`
    image is laid out as 3x3 periods, so every edge of the middle copy is
    upscaled with its true neighbours. The layout is shelf-packed, padded
    with black to an aspect between 1:2 and 2:1 (the upscaler refused a 6:1
    and a 1:4 input), and enlarged with Lanczos by the integer factor that
    brings the long side nearest `--long`. `<out>.json` records the layout
    for `pack`."""
    np, Image, _ = libs()
    inv = Path(args.inv)
    tiles = set(args.tile.split(",")) if args.tile else set()
    items = []
    for key in args.keys.split(","):
        g = find_group(inv, key)
        px = np.asarray(Image.open(inv / "images" / f"{g['key']}.png").convert("RGBA"))
        rgb = upscaler_rgb(np, px)
        tile = args.tile == "all" or key in tiles or g["key"] in tiles
        if tile:
            rgb = np.tile(rgb, (3, 3, 1))
        gut = args.gutter
        padded = np.pad(rgb, ((gut, gut), (gut, gut), (0, 0)), mode="wrap" if tile else "edge")
        items.append({"key": g["key"], "tile": tile, "rgb": padded, "gutter": gut})
    area = sum(i["rgb"].shape[0] * i["rgb"].shape[1] for i in items)
    width = max(i["rgb"].shape[1] for i in items)
    if len(items) > 1:
        width = max(width, int((area * 1.5) ** 0.5))
    x = y = shelf = 0
    for i in sorted(items, key=lambda i: -i["rgb"].shape[0]):
        h, w = i["rgb"].shape[:2]
        if x + w > width:
            x, y, shelf = 0, y + shelf, 0
        i["x"], i["y"] = x, y
        x += w
        shelf = max(shelf, h)
    height = y + shelf
    width = max(width, (height + 1) // 2)
    height = max(height, (width + 1) // 2)
    canvas = np.zeros((height, width, 3), np.uint8)
    for i in items:
        h, w = i["rgb"].shape[:2]
        canvas[i["y"]:i["y"] + h, i["x"]:i["x"] + w] = i["rgb"]
    factor = max(1, round(args.long / max(width, height)))
    img = Image.fromarray(canvas, "RGB").resize((width * factor, height * factor), Image.LANCZOS)
    out = Path(args.out)
    img.save(out)
    layout = {"inventory": str(inv.resolve()), "width": width, "height": height, "factor": factor,
              "items": [{k: i[k] for k in ("key", "tile", "x", "y", "gutter")} for i in items]}
    out.with_suffix(".json").write_text(json.dumps(layout, indent=1))
    print(f"hdtex.py: {len(items)} source image(s) in {width}x{height}, {factor}x -> "
          f"{img.width}x{img.height} {out}")
    return 0


def seamless_middle(np, big, w: int, h: int, band: int):
    """The middle period of a 3x3 upscale, made to wrap: within `band` of the
    left and top edges it cross-fades into the copies to the right and below,
    which the upscaler drew continuing the middle copy's right and bottom
    edges."""
    f = big.astype(np.float32)
    c = f[h:2 * h, w:2 * w]
    r = f[h:2 * h, 2 * w:3 * w]
    b = f[2 * h:3 * h, w:2 * w]
    br = f[2 * h:3 * h, 2 * w:3 * w]
    ax = np.clip(1 - np.arange(w) / band, 0, 1)[None, :, None]
    ay = np.clip(1 - np.arange(h) / band, 0, 1)[:, None, None]
    out = (1 - ax) * (1 - ay) * c + ax * (1 - ay) * r + (1 - ax) * ay * b + ax * ay * br
    return out.clip(0, 255).astype(np.uint8)


def cmd_pack(args) -> int:
    """Cut every piece of every source image in a `prep` layout out of the
    upscaled image and write the pieces as replacements under hd/<folder>."""
    np, Image, _ = libs()
    layout = json.loads(Path(args.layout).read_text())
    inv = Path(layout["inventory"])
    entries = {e["hash"]: e for e in json.loads((inv / "index.json").read_text())}
    src = Image.open(args.upscaled).convert("RGB")
    # 8x covers RESOLUTION 9x with room to spare; more only costs memory.
    scale = args.scale or min(args.max_scale, max(1, round(src.width / layout["width"])))
    big = np.asarray(src.resize((layout["width"] * scale, layout["height"] * scale), Image.LANCZOS))
    dest = HD / args.folder
    dest.mkdir(parents=True, exist_ok=True)
    written = []
    for item in layout["items"]:
        g = find_group(inv, item["key"])
        px = np.asarray(Image.open(inv / "images" / f"{g['key']}.png").convert("RGBA"))
        w, h = g["width"], g["height"]
        periods = 3 if item["tile"] else 1
        x0 = (item["x"] + item["gutter"]) * scale
        y0 = (item["y"] + item["gutter"]) * scale
        region = big[y0:y0 + periods * h * scale, x0:x0 + periods * w * scale]
        if item["tile"]:
            region = seamless_middle(np, region, w * scale, h * scale, 8 * scale)
        alpha = hd_alpha(np, Image, px[:, :, 3], scale)
        rgb = region.astype(np.float32)
        if alpha_kind(np, px[:, :, 3]) == "soft":
            a = alpha.astype(np.float32)[:, :, None] / 255.0
            rgb = np.where(a > 0.03, rgb / np.maximum(a, 0.03), 0)
        rgba = np.dstack([rgb.clip(0, 255).astype(np.uint8), alpha])
        save_master(np, Image, args.folder, g, px, rgba, scale)
        written += cut_pieces(np, Image, g, entries, rgba, scale, g["x0"], g["y0"], dest)
        print(f"hdtex.py: {g['key']}: {len(g['hashes'])} piece(s) at {scale}x")
    added = merge_pack(args.folder, written)
    print(f"hdtex.py: {len(added)} replacement(s) -> {dest}")
    return 0


def cut_pieces(np, Image, g: dict, entries: dict, master, scale: int, mx0: int, my0: int,
               dest: Path) -> list[str]:
    """Write each piece of source image `g` cut from `master`, an HD image
    whose texel (0, 0) is source texel (mx0, my0)."""
    written = []
    for hsh in g["hashes"]:
        e = entries[hsh]
        x, y = (e["uls"] - mx0) * scale, (e["ult"] - my0) * scale
        piece = master[y:y + e["height"] * scale, x:x + e["width"] * scale] if x >= 0 and y >= 0 else None
        if piece is None or piece.shape[:2] != (e["height"] * scale, e["width"] * scale):
            print(f"hdtex.py: {hsh} ({e['uls']},{e['ult']} {e['width']}x{e['height']}) falls "
                  f"outside the master of {g['key']}; skipped", file=sys.stderr)
            continue
        Image.fromarray(np.ascontiguousarray(piece), "RGBA").save(dest / f"{hsh}.png")
        written.append(hsh)
    return written


def save_master(np, Image, folder: str, g: dict, original, master, scale: int) -> None:
    out = HD_SRC / folder
    out.mkdir(parents=True, exist_ok=True)
    Image.fromarray(master, "RGBA").save(out / f"{g['key']}.png")
    Image.fromarray(original, "RGBA").save(out / f"{g['key']}.orig.png")
    meta = {k: g[k] for k in ("key", "x0", "y0", "width", "height", "fmt")}
    meta["image_width"] = g.get("image_width")
    meta["scale"] = scale
    (out / f"{g['key']}.json").write_text(json.dumps(meta, indent=1))


def match_master(np, Image, g: dict, inv: Path, meta: dict, orig) -> bool:
    """Whether source image `g` of a new scan is the image a master was made
    from: same format and row width, and equal texels wherever both have
    opaque texels (at least half of g's opaque texels must overlap)."""
    if g["fmt"] != meta["fmt"] or g.get("image_width") != meta.get("image_width"):
        return False
    px = np.asarray(Image.open(inv / "images" / f"{g['key']}.png").convert("RGBA"))
    ox, oy = g["x0"] - meta["x0"], g["y0"] - meta["y0"]
    h, w = px.shape[:2]
    if ox < 0 or oy < 0 or ox + w > orig.shape[1] or oy + h > orig.shape[0]:
        return False
    o = orig[oy:oy + h, ox:ox + w]
    both = (px[:, :, 3] > 0) & (o[:, :, 3] > 0)
    if both.sum() < 0.5 * max(1, (px[:, :, 3] > 0).sum()):
        return False
    return bool((px[both] == o[both]).all())


def locate(np, px, orig):
    """(x, y) of the first window of `orig` equal to `px` (RGBA), or None."""
    h, w = px.shape[:2]
    H, W = orig.shape[:2]
    if h > H or w > W:
        return None
    from numpy.lib.stride_tricks import sliding_window_view
    row0 = px[0]
    for y in range(H - h + 1):
        win = sliding_window_view(orig[y], (w, 4))[:, 0]
        for x in np.nonzero((win == row0).all(axis=(1, 2)))[0]:
            if (orig[y:y + h, x:x + w] == px).all():
                return int(x), int(y)
    return None


def cmd_repack(args) -> int:
    """Re-cut every master in hd-src/<folder> against a new inventory."""
    np, Image, _ = libs()
    inv = Path(args.inv)
    entries = {e["hash"]: e for e in json.loads((inv / "index.json").read_text())}
    groups = json.loads((inv / "images.json").read_text())
    for g in groups:
        g.setdefault("image_width", entries[g["hashes"][0]].get("image_width"))
    dest = HD / args.folder
    dest.mkdir(parents=True, exist_ok=True)
    written = []
    for mj in sorted((HD_SRC / args.folder).glob("*.json")):
        meta = json.loads(mj.read_text())
        orig = np.asarray(Image.open(mj.with_suffix(".orig.png")).convert("RGBA"))
        master = np.asarray(Image.open(mj.with_suffix(".png")).convert("RGBA"))
        hits = [g for g in groups if match_master(np, Image, g, inv, meta, orig)]
        for g in hits:
            written += cut_pieces(np, Image, g, entries, master, meta["scale"],
                                  meta["x0"], meta["y0"], dest)
        # Pieces with no RDRAM load of their own: find their texels in the
        # master's original and cut them from the same place.
        found = 0
        for g in groups:
            if not g["key"].startswith("tmem-") or g["fmt"] != meta["fmt"]:
                continue
            px = np.asarray(Image.open(inv / "images" / f"{g['key']}.png").convert("RGBA"))
            at = locate(np, px, orig)
            if at is None:
                continue
            e = entries[g["hashes"][0]]
            k = meta["scale"]
            piece = master[at[1] * k:(at[1] + e["height"]) * k, at[0] * k:(at[0] + e["width"]) * k]
            Image.fromarray(np.ascontiguousarray(piece), "RGBA").save(dest / f"{e['hash']}.png")
            written.append(e["hash"])
            found += 1
        print(f"hdtex.py: {meta['key']}: {len(hits)} source image(s) in this scan, "
              f"{sum(len(g['hashes']) for g in hits)} piece(s), {found} TMEM-only piece(s) found in it")
    added = merge_pack(args.folder, sorted(set(written)))
    print(f"hdtex.py: {len(added)} replacement(s) -> {dest}")
    return 0


def cmd_merge(args) -> int:
    """Merge several scans into one inventory, so a source image seen in two
    runs (WIDESCREEN off and on) keeps every piece."""
    np, Image, _ = libs()
    out = Path(args.out)
    (out / "png").mkdir(parents=True, exist_ok=True)
    seen: dict[str, dict] = {}
    for inv in args.invs:
        inv = Path(inv)
        for e in json.loads((inv / "index.json").read_text()):
            if e["hash"] in seen or not e.get("decoded"):
                continue
            seen[e["hash"]] = e
            (out / "png" / f"{e['hash']}.png").write_bytes((inv / "png" / f"{e['hash']}.png").read_bytes())
    entries = sorted(seen.values(), key=lambda e: (e["address"], e["ult"], e["uls"], e["hash"]))
    for n, e in enumerate(entries):
        e["n"] = n
    (out / "index.json").write_text(json.dumps(entries, indent=1))
    groups = assemble(entries, out)
    (out / "images.json").write_text(json.dumps(groups, indent=1))
    print(f"hdtex.py: {len(entries)} textures, {len(groups)} source images -> {out}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("dump")
    s.add_argument("-o", "--out", required=True)
    m = sub.add_parser("merge")
    m.add_argument("invs", nargs="+")
    m.add_argument("-o", "--out", required=True)
    pr = sub.add_parser("prep")
    pr.add_argument("inv")
    pr.add_argument("keys", help="source image keys from images.json (or unique prefixes), comma separated")
    pr.add_argument("-o", "--out", required=True)
    pr.add_argument("--long", type=int, default=1024)
    pr.add_argument("--gutter", type=int, default=4)
    pr.add_argument("--tile", help="keys of wrapping textures (comma separated), or 'all'")
    pk = sub.add_parser("pack")
    pk.add_argument("layout", help="the .json that prep wrote beside its image")
    pk.add_argument("upscaled")
    pk.add_argument("--folder", required=True)
    pk.add_argument("--scale", type=int)
    pk.add_argument("--max-scale", type=int, default=8)
    rp = sub.add_parser("repack")
    rp.add_argument("inv")
    rp.add_argument("--folder", required=True)
    args = p.parse_args()
    return {"scan": cmd_scan, "merge": cmd_merge, "prep": cmd_prep, "pack": cmd_pack,
            "repack": cmd_repack}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
