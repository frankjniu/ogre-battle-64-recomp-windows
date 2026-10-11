"""Insert the app's mouse-driven world-map cursor into the generated map module.

The world map (scene 0x05) keeps its cursor position as screen pixels in its
state object at `9*(u32*)0x80197B18`:

    state[+0x50] (u8)   1 = draw the cursor
    state[+0x52] (s16)  cursor x
    state[+0x54] (s16)  cursor y

The scene hook's cursor draw (`func_ovlM_801A103C`) reads that pair at
0x801A1C48/0x801A1C4C and emits the sprite there:

    0x801A1C34  lw   $v1, 0x7B18($v1)    # $v1 = the map state
    0x801A1C3C  lbu  $v0, 0x50($v1)      # 1 = draw
    0x801A1C40  beq  $v0, $zero, skip
    0x801A1C48  lh   $a0, 0x52($v1)      # x
    0x801A1C4C  lh   $a1, 0x54($v1)      # y
    0x801A1C50  jal  func_ovlM_8019F83C  # emit sprite, a2 = 6 = the cursor

The call is inserted after 0x801A1C40's branch, immediately before the x/y load,
so it runs only on the frame's cursor-draw path and its write reaches the read
below it. The app side is `app/src/map_cursor.cpp`; it no-ops when the mouse has
not moved or `OGRE_MAP_CURSOR` is off.

Bank units are outside the mod hook map (a `.nrm` hook resolves only through the
base ELF's sections), so this insertion is the only place to hook it. It is a
code *generation* fix, like `tools/njpeg_readback.py`: `make bank-recomp`
regenerates `BankMFuncs/`, so `Makefile` re-applies it on every run.

Usage:
    python3 tools/map_cursor.py           # apply (idempotent)
    python3 tools/map_cursor.py --revert  # remove
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "BankMFuncs" / "funcs_0.c"

# The x load of the cursor draw. The jump-table data at 0x801A7F80 has an entry
# that prints as `0x7F80($at)`, not `0X52(`, and the string occurs nowhere else
# in the generated unit.
ANCHOR = (
    "    // 0x801A1C48: lh          $a0, 0x52($v1)\n"
    "    ctx->r4 = MEM_H(ctx->r3, 0X52);\n"
)

CALL_MARKER = "// map cursor: place the sprite at the pointer (app/src/map_cursor.cpp)."

CALL = (
    "    " + CALL_MARKER + "\n"
    "    // `ctx->r3` is `*(u32*)0x80197B18` at this point in the function.\n"
    "    ogre_map_cursor(rdram, (int32_t)ctx->r3);\n"
)

DECL_MARKER = "// map cursor: the app drives the world map's cursor (scene 0x05)."

# `#include "funcs.h"` occurs once in the generated unit (line 2).
DECL_ANCHOR = '#include "funcs.h"\n'

DECL = (
    DECL_ANCHOR
    + "\n"
    + DECL_MARKER
    + "\n"
    + "#ifdef __cplusplus\n"
    + 'extern "C" void ogre_map_cursor(uint8_t* rdram, int32_t state);\n'
    + "#else\n"
    + "void ogre_map_cursor(uint8_t* rdram, int32_t state);\n"
    + "#endif\n"
)

APPLIED = "    " + CALL_MARKER + "\n    // `ctx->r3` is `*(u32*)0x80197B18` at this point in the function.\n    ogre_map_cursor(rdram, (int32_t)ctx->r3);\n"

DECL_APPLIED = (
    "\n"
    + DECL_MARKER
    + "\n#ifdef __cplusplus\nextern \"C\" void ogre_map_cursor(uint8_t* rdram, int32_t state);\n"
    + "#else\nvoid ogre_map_cursor(uint8_t* rdram, int32_t state);\n#endif\n"
)


def apply() -> int:
    src = TARGET.read_text()
    if CALL_MARKER in src:
        print("map_cursor: already applied")
        return 0
    if src.count(ANCHOR) != 1:
        print(
            f"map_cursor: the anchor 0x801A1C48 occurs {src.count(ANCHOR)} times in "
            f"{TARGET.relative_to(ROOT)}; the generated map module changed. Update this "
            "script rather than editing the generated C.",
            file=sys.stderr,
        )
        return 1
    if src.count(DECL_ANCHOR) != 1:
        print(
            f"map_cursor: the declaration anchor occurs {src.count(DECL_ANCHOR)} times in "
            f"{TARGET.relative_to(ROOT)}.",
            file=sys.stderr,
        )
        return 1
    src = src.replace(DECL_ANCHOR, DECL, 1)
    src = src.replace(ANCHOR, CALL + ANCHOR, 1)
    TARGET.write_text(src)
    print(f"map_cursor: applied to {TARGET.relative_to(ROOT)}")
    return 0


def revert() -> int:
    src = TARGET.read_text()
    if CALL_MARKER not in src:
        print("map_cursor: not applied")
        return 0
    src = src.replace(APPLIED, "", 1)
    src = src.replace(DECL_APPLIED, "", 1)
    TARGET.write_text(src)
    print(f"map_cursor: reverted {TARGET.relative_to(ROOT)}")
    return 0


def main() -> int:
    if "--revert" in sys.argv[1:]:
        return revert()
    if not TARGET.is_file():
        print(
            f"map_cursor: {TARGET.relative_to(ROOT)} does not exist; run `make bank-recomp` "
            "first.",
            file=sys.stderr,
        )
        return 1
    return apply()


if __name__ == "__main__":
    raise SystemExit(main())
