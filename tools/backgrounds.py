#!/usr/bin/env python3
"""Extract the game's own backdrop, and build the HD texture pack for a backdrop.

The port writes the reference itself: with `OGRE_BG_DUMP=<dir>` set,
`app/src/hd_backgrounds.cpp` assembles the game's four `njpeg` sub-images into
the 496x384 backdrop canvas and writes one PNG per assembly, before the
`mods/backgrounds/` pack replaces anything. One run dumps every backdrop it
assembles, so the switch covers later backgrounds with no code change.

This tool drives that run and files the result:

    tools/backgrounds.py list
    tools/backgrounds.py extract 01            # the cathedral, from backgrounds.txt
    tools/backgrounds.py extract 01 --step 6   # another step of scene 0x0D
    tools/backgrounds.py extract --all         # a plain boot, keep every dump
    tools/backgrounds.py extract 01 --from /tmp/dump   # file an existing dump

Output defaults to `mods/backgrounds/reference/<id>-<name>-original.png`. The
port ignores that subdirectory when it loads the pack, so a reference cannot be
mistaken for a replacement.

`hd <id>` builds the HD strips for one mapping:

    tools/backgrounds.py hd 01                 # the cathedral, plain and -wide
    tools/backgrounds.py hd 01 --scale 4       # force the HD canvas to 4x
    tools/backgrounds.py hd 01 --dump D        # keep the RT64 texture dumps in D
    tools/backgrounds.py hd 01 --operation stream   # load strips in the background
    tools/backgrounds.py hd 01 --from D        # reuse D instead of running

The game draws the composed backdrop as textured quads, one `G_LOADTILE` strip
per quad (320x6 or 176x11 texels from the four canvas images), and RT64 keys a
replacement by the TMEM hash of each strip. The command runs the app on the
mapping's route with the pack applied and `OGRE_DUMP_TEX` set (once more with
WIDESCREEN on when a `-wide` image exists), reads the hash, source address and
rows of every strip from RT64's dump, checks each strip's pixels against the
canvas the pack wrote, cuts the same rectangle out of the source PNG at an
integer scale, and writes `mods/backgrounds/hd/<id>[-wide]/<hash>.png` plus
`mods/backgrounds/hd/rt64.json`. The app loads `hd/` as an RT64 texture pack.
Needs Pillow.

The runs need the app and the ROM. Both commands build the route from the
mapping: scene `0x0D` is the New Game dialogue scene, reached with
`OGRE_SCENE=new-game` and `OGRE_STEP=<step>`; any other scene is forced with
`OGRE_SCENE`.

Usage: see `docs/guides/hd-backgrounds.md`.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACK = ROOT / "mods" / "backgrounds"
MAPPING = PACK / "backgrounds.txt"
REFERENCE = PACK / "reference"
HD = PACK / "hd"
DEFAULT_ROM = Path(os.environ["OGRE_ROM"]) if os.environ.get("OGRE_ROM") else ROOT / "assets" / "ogre64.z64"

# `make app` writes `build-dist/` when the static SDL2 build is the default;
# `cmake --build build-app` writes `build-app/`. Use whichever exists. On
# Windows, `build-dist/ogrebattle64.exe` needs `dxcompiler.dll` beside it, and
# the packaged copy from `make dist` has it.
APP_CANDIDATES = [
    ROOT / "build-app" / "ogrebattle64",
    ROOT / "build-dist" / "ogrebattle64",
    ROOT / "build-app" / "Release" / "ogrebattle64",
    ROOT / "build-dist" / "Release" / "ogrebattle64",
]
if os.name == "nt":
    APP_CANDIDATES = [
        ROOT / "dist" / "ogre-battle-64-recomp" / "ogrebattle64.exe",
        ROOT / "build-dist" / "ogrebattle64.exe",
        ROOT / "build-app" / "ogrebattle64.exe",
    ]


def default_app() -> Path:
    for candidate in APP_CANDIDATES:
        if candidate.is_file():
            return candidate
    return APP_CANDIDATES[0]

# Scene 0x0D is the dialogue/cutscene scene the New Game opening drives with
# OGRE_SCENE=new-game and OGRE_STEP=<step>. Any other scene has no such route, so
# the tool forces it and warns.
DIALOGUE_SCENE = 0x0D

# At OGRE_SPEED=8 the boot plus the preload reaches the first dialogue in a few
# wall-clock seconds; 20 s leaves room for a slow machine.
RUN_SECONDS = 20


def read_mapping(path: Path) -> list[tuple[str, str, int, int]]:
    entries: list[tuple[str, str, int, int]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0]
        fields = line.split()
        if len(fields) != 4:
            continue
        eid, name, scene, step = fields
        entries.append((eid, name, int(scene, 0) & 0x7FFF, int(step, 0) & 0x7FFF))
    return entries


def find_entry(eid: str) -> tuple[str, str, int, int]:
    for entry in read_mapping(MAPPING):
        if entry[0] == eid:
            return entry
    raise SystemExit(
        f"backgrounds.py: '{eid}' is not in {MAPPING.relative_to(ROOT)}; "
        f"run 'backgrounds.py list'"
    )


def run_dump(app: Path, rom: Path, dump_dir: Path, scene: int, step: int) -> None:
    run_route(app, rom, scene, step, {"OGRE_BG_DUMP": str(dump_dir)},
              f"OGRE_BG_DUMP={dump_dir}")


def run_route(app: Path, rom: Path, scene: int, step: int, extra: dict[str, str],
              what: str, log: Path | None = None) -> None:
    if not app.is_file():
        raise SystemExit(f"backgrounds.py: no app at {app}; build it with 'make app'")
    if not rom.is_file():
        raise SystemExit(f"backgrounds.py: no ROM at {rom}; pass --rom <path>")

    env = dict(os.environ)
    env.update(extra)
    env["OGRE_NO_AUDIO"] = "1"
    # Any install with .nrm mods shows the start screen; accept the ROM.
    env["OGRE_LAUNCHER_KEYS"] = "space"
    env["OGRE_NJPEG"] = "1"
    env["OGRE_SPEED"] = "8"
    env["OGRE_EXIT_AFTER_MS"] = str(RUN_SECONDS * 1000)
    if scene == DIALOGUE_SCENE:
        env["OGRE_SCENE"] = "new-game"
        env["OGRE_STEP"] = str(step)
    else:
        env["OGRE_SCENE"] = hex(scene)
        env.pop("OGRE_STEP", None)
        print(
            f"backgrounds.py: scene 0x{scene:02X} has no built-in route; forcing it. "
            f"A forced scene enters without the pre-state a natural path builds, so "
            f"the dump may be empty (docs/guides/app-build.md -> scene forcing).",
            file=sys.stderr,
        )

    print(f"backgrounds.py: running {app.name} with {what}")
    if log is None:
        subprocess.run([str(app), str(rom)], env=env, check=False)
        return
    with log.open("wb") as out:
        subprocess.run([str(app), str(rom)], env=env, check=False, stdout=out,
                       stderr=subprocess.STDOUT)


def file_dumps(dump_dir: Path, out_dir: Path, wanted: list[tuple[str, str, int, int]]) -> int:
    found = sorted(dump_dir.glob("original-step*.png"))
    if not found:
        print(
            f"backgrounds.py: no original-step*.png in {dump_dir}\n"
            f"  the run did not assemble a backdrop; check that it reached the scene\n"
            f"  and that the app was built with the HD-backgrounds hook",
            file=sys.stderr,
        )
        return 1
    out_dir.mkdir(parents=True, exist_ok=True)
    wanted_by_step = {step: (eid, name) for eid, name, _scene, step in wanted}
    written = 0
    for png in found:
        step = int(png.stem.removeprefix("original-step"))
        eid, name = wanted_by_step.get(step, (None, None))
        if eid is None:
            dest = out_dir / f"step{step:04d}-original.png"
        else:
            dest = out_dir / f"{eid}-{name}-original.png"
        dest.write_bytes(png.read_bytes())
        try:
            shown = dest.relative_to(ROOT)
        except ValueError:
            shown = dest
        print(f"backgrounds.py: {shown}  (from {png.name})")
        written += 1
    return 0


# --- HD texture pack (`hd <id>`) -------------------------------------------

CANVAS_W, CANVAS_H = 496, 384
# The four canvas images by (width, height) and their canvas origin. The sizes
# are distinct, so a strip's source image names its place. The bottom images
# start at row 239, over the top images' last row (`chunk_rect` in
# app/src/hd_backgrounds.cpp, where the measurement is described).
CANVAS_IMAGES = {
    (320, 240): (0, 0),
    (176, 240): (320, 0),
    (320, 144): (0, 239),
    (176, 144): (320, 239),
}
# A strip's pixels must equal the canvas the pack wrote for at least this share
# of its rows inside the image; the rest is the check's margin for a source PNG
# that Pillow and stb_image decode differently (16-bit or gamma-tagged files).
MATCH_SHARE = 0.98


def pillow():
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("backgrounds.py: 'hd' needs Pillow (pip install pillow)")
    return Image


def entry_sources(eid: str, name: str) -> dict[str, Path]:
    """The entry's images: 'plain' (`<id>.png`) and 'wide' (`<id>-wide.png`)."""
    found: dict[str, Path] = {}
    for stem in (eid, name):
        if "plain" not in found and (PACK / f"{stem}.png").is_file():
            found["plain"] = PACK / f"{stem}.png"
        if "wide" not in found and (PACK / f"{stem}-wide.png").is_file():
            found["wide"] = PACK / f"{stem}-wide.png"
    return found


def cpp_lround(x: float) -> int:
    return int(x + 0.5) if x >= 0 else -int(-x + 0.5)


def canvas_layout(sw: int, sh: int, fit: bool) -> tuple[int, int, int, int]:
    """(width, height, x, y) of the image inside the 1x canvas, as
    `to_canvas`/`to_canvas_fit` in app/src/hd_backgrounds.cpp place it."""
    if not fit:
        return CANVAS_W, CANVAS_H, 0, 0
    dw = CANVAS_W
    dh = max(1, cpp_lround(CANVAS_W * sh / sw))
    if dh > CANVAS_H:
        dh = CANVAS_H
        dw = max(1, cpp_lround(CANVAS_H * sw / sh))
    return dw, dh, (CANVAS_W - dw) // 2, CANVAS_H - dh


def lowres_canvas(img, fit: bool) -> list[list[tuple[int, int, int]]]:
    """The 496x384 canvas of 5-bit RGB the port writes over the game's
    backdrop: the box filter and rounding of `resample` in hd_backgrounds.cpp."""
    sw, sh = img.size
    px = img.convert("RGBA").tobytes()
    dw, dh, x0, y0 = canvas_layout(sw, sh, fit)
    small: list[list[tuple[int, int, int]]] = []
    for dy in range(dh):
        ya = dy * sh // dh
        yb = max(ya + 1, (dy + 1) * sh // dh)
        row_out = []
        for dx in range(dw):
            xa = dx * sw // dw
            xb = max(xa + 1, (dx + 1) * sw // dw)
            r = g = b = n = 0
            for sy in range(ya, yb):
                o = (sy * sw + xa) * 4
                for _ in range(xa, xb):
                    r += px[o]
                    g += px[o + 1]
                    b += px[o + 2]
                    n += 1
                    o += 4
            row_out.append(((r // n * 31 + 127) // 255, (g // n * 31 + 127) // 255,
                            (b // n * 31 + 127) // 255))
        small.append(row_out)
    canvas = []
    for y in range(CANVAS_H):
        src = small[min(max(y - y0, 0), dh - 1)]
        canvas.append([src[min(max(x - x0, 0), dw - 1)] for x in range(CANVAS_W)])
    return canvas


def strip_stride(width: int) -> int:
    """Rows between two strips of a chunk: a 320-wide chunk loads 6 rows every
    5, a 176-wide chunk 11 rows every 10 (one overlap row)."""
    return 5 if width == 320 else 10


def make_strips_unique(canvas: list[list[tuple[int, int, int]]]) -> int:
    """`make_strips_unique` in app/src/hd_backgrounds.cpp, in the same order:
    a strip whose pixels repeat an earlier strip's gets the lowest blue bit of
    one texel in its third row flipped, at the first column that makes it
    unique. Returns how many strips were changed."""
    seen: set[tuple] = set()
    changed = 0
    for (w, h), (cx, cy) in CANVAS_IMAGES.items():
        stride = strip_stride(w)
        for top in range(0, h, stride):
            rows = min(stride + 1, h - top)
            mark = cy + top + min(2, rows - 1)

            def key():
                return (w, rows) + tuple(tuple(canvas[cy + top + r][cx:cx + w]) for r in range(rows))

            k = key()
            col = -1
            while k in seen:
                if col >= 0:
                    r, g, b = canvas[mark][cx + col]
                    canvas[mark][cx + col] = (r, g, b ^ 1)
                col += 1
                if col >= w:
                    break
                r, g, b = canvas[mark][cx + col]
                canvas[mark][cx + col] = (r, g, b ^ 1)
                k = key()
            if col >= 0:
                changed += 1
            seen.add(k)
    return changed


def hd_canvas(img, fit: bool, scale: int):
    """The same placement as `lowres_canvas` at `scale` x the canvas, resampled
    with Lanczos instead of the box filter."""
    Image = pillow()
    sw, sh = img.size
    dw, dh, x0, y0 = canvas_layout(sw, sh, fit)
    k = scale
    scaled = img.convert("RGB").resize((dw * k, dh * k), Image.LANCZOS)
    if (dw, dh) == (CANVAS_W, CANVAS_H):
        return scaled
    canvas = Image.new("RGB", (CANVAS_W * k, CANVAS_H * k))
    canvas.paste(scaled, (x0 * k, y0 * k))
    # Rows above the image repeat its first row, columns beside it its edge
    # columns, as in to_canvas_fit.
    if y0 > 0:
        canvas.paste(scaled.crop((0, 0, dw * k, 1)).resize((dw * k, y0 * k)), (x0 * k, 0))
    if x0 > 0:
        left = canvas.crop((x0 * k, 0, x0 * k + 1, CANVAS_H * k))
        canvas.paste(left.resize((x0 * k, CANVAS_H * k)), (0, 0))
        right_x = (x0 + dw) * k
        right = canvas.crop((right_x - 1, 0, right_x, CANVAS_H * k))
        canvas.paste(right.resize((CANVAS_W * k - right_x, CANVAS_H * k)), (right_x, 0))
    return canvas


def default_scale(img, fit: bool) -> int:
    sw, sh = img.size
    dw, _dh, _x, _y = canvas_layout(sw, sh, fit)
    return max(1, int(sw / dw + 0.5))


def read_strips(dump: Path) -> tuple[dict[tuple[int, int], tuple[int, list[dict]]], int]:
    """The strips of the four canvas images in an RT64 texture dump
    (`OGRE_DUMP_TEX`), keyed by image size, and the dump's hash version.

    RT64 writes one `<hash>.v<n>.rice.json` per hash with the load that filled
    TMEM (`texture.address`, `tile.uls/ult/lrs/lrt` in 10.2 fixed point), and a
    `.tile.json` with the size of the texture the hash covers. A strip's texel
    (0, 0) is the load's first texel."""
    groups: dict[tuple[int, int], list[dict]] = {}
    version = 0
    for rice in dump.glob("*.rice.json"):
        base = rice.name[: -len(".rice.json")]
        tile_json = dump / f"{base}.tile.json"
        if not tile_json.is_file():
            continue
        r = json.loads(rice.read_text())
        t = json.loads(tile_json.read_text())
        tex, lt = r["texture"], r["tile"]
        if r.get("type") != "Tile" or tex["fmt"] != 0 or tex["siz"] != 2:
            continue
        if tex["width"] not in (320, 176):
            continue
        hash_hex, _, ver = base.partition(".v")
        version = max(version, int(ver or 0))
        groups.setdefault((tex["address"], tex["width"]), []).append({
            "hash": hash_hex,
            "address": tex["address"],
            "uls": lt["uls"] >> 2,
            "ult": lt["ult"] >> 2,
            "width": t["width"],
            "height": t["height"],
            "rdram": dump / f"{base}.rice.rdram",
        })
    images: dict[tuple[int, int], tuple[int, list[dict]]] = {}
    for (address, width), strips in groups.items():
        last_row = max(s["ult"] + s["height"] - 1 for s in strips)
        # A 240-row image's last strip loads one row past its end (the game's
        # one-row overlap), so its loads reach row 240.
        height = 240 if last_row in (239, 240) else 144 if last_row == 143 else 0
        if (width, height) not in CANVAS_IMAGES or min(s["ult"] for s in strips) != 0:
            continue
        if (width, height) in images and len(images[(width, height)][1]) >= len(strips):
            continue
        images[(width, height)] = (address, strips)
    return images, version


def strip_pixels(strip: dict, rows: int) -> list[list[tuple[int, int, int]]] | None:
    """The strip's RGBA5551 texels as 5-bit RGB, from RT64's RDRAM dump. The
    dump is host RDRAM: the guest byte at offset `j` of a word-aligned range is
    at `j ^ 3`."""
    if strip["address"] % 4 != 0 or not strip["rdram"].is_file():
        return None
    data = strip["rdram"].read_bytes()
    w = strip["width"]
    out = []
    for row in range(rows):
        line = []
        for col in range(w):
            p = (row * w + col) * 2
            if p + 1 >= len(data):
                return None
            v = (data[p ^ 3] << 8) | data[(p + 1) ^ 3]
            line.append(((v >> 11) & 31, (v >> 6) & 31, (v >> 1) & 31))
        out.append(line)
    return out


def build_variant(variant: str, source: Path, dump: Path, scale: int | None
                  ) -> tuple[list[tuple[str, object]], int]:
    """Check one dump against the canvas the pack wrote and cut the HD strips.
    Returns [(hash, image)] and the dump's hash version."""
    Image = pillow()
    fit = variant == "wide"
    img = Image.open(source)
    images, version = read_strips(dump)
    missing = [size for size in CANVAS_IMAGES if size not in images]
    if missing:
        raise SystemExit(
            f"backgrounds.py: {variant}: the dump in {dump} has no strips of the "
            f"{', '.join(f'{w}x{h}' for w, h in missing)} canvas image(s); the run did not "
            f"draw the backdrop (see {dump / 'run.log'})")
    k = scale or default_scale(img, fit)
    if k < 2:
        print(f"backgrounds.py: {variant}: {source.name} is {img.size[0]}x{img.size[1]}, "
              f"which gives a 1x canvas; the strips add no detail", file=sys.stderr)
    canvas = lowres_canvas(img, fit)
    changed = make_strips_unique(canvas)
    hd = hd_canvas(img, fit, k)

    out: list[tuple[str, object]] = []
    expected_total = found_total = 0
    for (w, h), (address, strips) in sorted(images.items()):
        cx, cy = CANVAS_IMAGES[(w, h)]
        equal = total = 0
        for s in strips:
            x, y = cx + s["uls"], cy + s["ult"]
            sw_, sh_ = s["width"], s["height"]
            if x + sw_ > CANVAS_W or y + sh_ > CANVAS_H:
                raise SystemExit(f"backgrounds.py: strip {s['hash']} ({sw_}x{sh_} at "
                                 f"canvas {x},{y}) leaves the canvas")
            rows_inside = min(sh_, h - s["ult"])
            pixels = strip_pixels(s, rows_inside)
            if pixels is not None:
                for row in range(rows_inside):
                    want = canvas[y + row][x:x + sw_]
                    equal += sum(1 for a, b in zip(pixels[row], want) if a == b)
                    total += sw_
            crop = hd.crop((x * k, y * k, (x + sw_) * k, (y + sh_) * k))
            out.append((s["hash"], crop))
        share = equal / total if total else 0.0
        print(f"backgrounds.py: {variant}: {w}x{h} image at 0x{address:08X} -> canvas "
              f"({cx},{cy}): {len(strips)} strips, {share:.1%} of texels match the pack")
        if share < MATCH_SHARE:
            raise SystemExit(
                f"backgrounds.py: {variant}: the {w}x{h} image's strips do not match canvas "
                f"rect ({cx},{cy}) of {source.name}; this backdrop's layout differs from the "
                f"four-chunk canvas, or the run did not apply the pack")
        # RT64 dumps one file per hash, so a strip position missing from the
        # dump shares its hash with another strip and would show that strip's
        # HD image.
        expected = set(range(0, h, strip_stride(w)))
        shared = sorted(expected - {s["ult"] for s in strips})
        expected_total += len(expected)
        found_total += len(expected) - len(shared)
        if shared:
            print(f"backgrounds.py: {variant}: the {w}x{h} image's strips at rows "
                  f"{', '.join(str(r) for r in shared)} share a hash with another strip; the "
                  f"app that wrote the dump does not make the strips unique "
                  f"(make_strips_unique in app/src/hd_backgrounds.cpp)", file=sys.stderr)
    print(f"backgrounds.py: {variant}: {found_total} of {expected_total} strips have their own "
          f"hash ({changed} made unique by one texel)")
    print(f"backgrounds.py: {variant}: {source.name} {img.size[0]}x{img.size[1]} -> HD canvas "
          f"{CANVAS_W * k}x{CANVAS_H * k} ({k}x)")
    return out, version


def write_pack(eid: str, variants: dict[str, list[tuple[str, object]]], version: int,
               operation: str) -> None:
    """Write the strips under hd/<id>[-wide]/ and merge them into hd/rt64.json,
    keeping the other mappings' entries."""
    HD.mkdir(parents=True, exist_ok=True)
    db_path = HD / "rt64.json"
    db = {"configuration": {}, "textures": []}
    if db_path.is_file():
        db = json.loads(db_path.read_text(encoding="utf-8"))
    db["configuration"] = {
        "autoPath": "rt64",
        "configurationVersion": 3,
        "defaultOperation": operation,
        "defaultShift": "half",
        "hashVersion": version,
    }
    folders = {"plain": eid, "wide": f"{eid}-wide"}
    mine = tuple(f"{folder}/" for folder in folders.values())
    textures = [t for t in db.get("textures", []) if not t.get("path", "").startswith(mine)]
    taken = {t["hashes"]["rt64"]: t["path"] for t in textures}
    for variant, strips in variants.items():
        folder = HD / folders[variant]
        if folder.is_dir():
            shutil.rmtree(folder)
        folder.mkdir(parents=True)
        for hash_hex, crop in strips:
            rel = f"{folders[variant]}/{hash_hex}.png"
            if hash_hex in taken:
                print(f"backgrounds.py: hash {hash_hex} is already {taken[hash_hex]}; "
                      f"skipping {rel}", file=sys.stderr)
                continue
            crop.save(folder / f"{hash_hex}.png")
            textures.append({"path": rel, "hashes": {"rt64": hash_hex, "rice": ""}})
            taken[hash_hex] = rel
        print(f"backgrounds.py: {folder.relative_to(ROOT)}: {len(strips)} strip(s)")
    db["textures"] = textures
    db_path.write_text(json.dumps(db, indent=1) + "\n", encoding="utf-8")
    print(f"backgrounds.py: {db_path.relative_to(ROOT)}: {len(textures)} texture(s)")


def cmd_hd(args) -> int:
    eid, name, scene, step = find_entry(args.id)
    sources = entry_sources(eid, name)
    if "plain" not in sources:
        raise SystemExit(f"backgrounds.py: no {eid}.png or {name}.png in {PACK}")
    if args.from_dir is not None:
        dump_root = args.from_dir
    elif args.dump is not None:
        dump_root = args.dump
    else:
        import tempfile

        dump_root = Path(tempfile.mkdtemp(prefix="ogre-bg-hd-"))
    variants: dict[str, list[tuple[str, object]]] = {}
    version = 0
    for variant, source in sources.items():
        dump = dump_root / variant
        if args.from_dir is None:
            if dump.is_dir():
                shutil.rmtree(dump)
            dump.mkdir(parents=True)
            run_route(args.app, args.rom, scene, step, {
                "OGRE_DUMP_TEX": str(dump),
                "OGRE_BG": "1",
                "OGRE_BG_DIR": str(PACK),
                "OGRE_BG_HD": "0",
                "OGRE_WIDESCREEN": "on" if variant == "wide" else "off",
            }, f"OGRE_DUMP_TEX={dump}" + (" and WIDESCREEN on" if variant == "wide" else ""),
                log=dump / "run.log")
        strips, version = build_variant(variant, source, dump, args.scale)
        variants[variant] = strips
    write_pack(eid, variants, version, args.operation)
    if args.from_dir is None and args.dump is None:
        shutil.rmtree(dump_root, ignore_errors=True)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(prog="backgrounds.py", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="print the pack's mappings")

    ex = sub.add_parser("extract", help="dump and file the game's own backdrop")
    ex.add_argument("id", nargs="?", help="the mapping id, e.g. 01")
    ex.add_argument("--all", action="store_true", help="a plain boot; keep every dump")
    ex.add_argument("--from", dest="from_dir", type=Path,
                    help="file an existing dump directory instead of running")
    ex.add_argument("--scene", type=lambda s: int(s, 0), help="override the mapped scene")
    ex.add_argument("--step", type=lambda s: int(s, 0), help="override the mapped step")
    ex.add_argument("--out", type=Path, default=REFERENCE, help="output directory")
    ex.add_argument("--app", type=Path, default=None)
    ex.add_argument("--rom", type=Path, default=DEFAULT_ROM)
    ex.add_argument("--dump", type=Path, help="keep the raw dump here (default: a temp dir)")

    hd = sub.add_parser("hd", help="build the HD texture pack strips for one mapping")
    hd.add_argument("id", help="the mapping id, e.g. 01")
    hd.add_argument("--scale", type=int, default=None,
                    help="HD canvas scale (default: the source width over the canvas width, rounded)")
    hd.add_argument("--dump", type=Path,
                    help="keep the RT64 texture dumps here, as <dir>/plain and <dir>/wide")
    hd.add_argument("--from", dest="from_dir", type=Path,
                    help="reuse the dumps of an earlier --dump instead of running")
    hd.add_argument("--app", type=Path, default=None)
    hd.add_argument("--rom", type=Path, default=DEFAULT_ROM)
    # stall: RT64 decodes a strip before the frame that first draws it, so no
    # strip shows at game resolution; the cathedral's 116 strips cost 16.1 ms
    # once. stream: background threads load them and the game-resolution strip
    # draws meanwhile; all 116 were in HD 21.1 ms after the first was queued
    # (normal speed, 2026-10-31).
    hd.add_argument("--operation", choices=("stall", "stream"), default="stall",
                    help="RT64 load operation for the strips (default: stall)")

    args = ap.parse_args()
    if args.command in ("extract", "hd") and args.app is None:
        args.app = default_app()

    if args.command == "list":
        for eid, name, scene, step in read_mapping(MAPPING):
            print(f"{eid}  {name:<12} scene=0x{scene:02X} step={step}")
        return 0

    if args.command == "hd":
        return cmd_hd(args)

    # extract
    if args.all and args.id:
        raise SystemExit("backgrounds.py: --all takes no id")
    if not args.all and not args.id:
        raise SystemExit("backgrounds.py: pass an id (see 'backgrounds.py list') or --all")

    wanted: list[tuple[str, str, int, int]] = []
    if args.id:
        wanted = [find_entry(args.id)]

    if args.from_dir is not None:
        if not args.from_dir.is_dir():
            raise SystemExit(f"backgrounds.py: {args.from_dir} is not a directory")
        return file_dumps(args.from_dir, args.out, wanted)

    if args.dump is not None:
        dump_dir = args.dump
        dump_dir.mkdir(parents=True, exist_ok=True)
    else:
        import tempfile

        dump_dir = Path(tempfile.mkdtemp(prefix="ogre-bg-dump-"))

    if args.all:
        # A plain boot assembles whatever the run reaches.
        scene = args.scene if args.scene is not None else DIALOGUE_SCENE
        step = args.step if args.step is not None else 0
    else:
        _eid, _name, mapped_scene, mapped_step = wanted[0]
        scene = args.scene if args.scene is not None else mapped_scene
        step = args.step if args.step is not None else mapped_step

    run_dump(args.app, args.rom, dump_dir, scene, step)
    return file_dumps(dump_dir, args.out, wanted)


if __name__ == "__main__":
    sys.exit(main())
