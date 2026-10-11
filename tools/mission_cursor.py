"""Insert the app's mouse-driven mission cursor into the generated mission module.

The mission field (scene 0x03) keeps its cursor as two f32 screen pixels in the
state object at the fixed address 0x800E91D0, which record 7 reaches through its
own data word `*(u32*)0x801AB870`:

    state[+0x08] (f32)  cursor x, framebuffer pixels (320x240, centre 160)
    state[+0x0C] (f32)  cursor y, framebuffer pixels (320x240, centre 120)

The mover `func_ovlN_801BE4E0` reads the held button word `*(0x800E8108)` and
the profile's D-pad masks, integrates a per-frame delta and stores the pair at
0x801BE974 / 0x801BE988, then clamps it to x 40.0..280.0 (0x801BEB98 /
0x801BEBB8) and y 30.0..210.0 (0x801BEBE0 / 0x801BEC00):

    0x801BEBB8  swc1  $f2, 0x8($v0)      # x clamp, low
    0x801BEC00  swc1  $f2, 0xC($v0)      # y clamp, low
    0x801BEC04                           # both clamps converge here
    ...
    L_801BECC8                           # the epilogue, reached by both paths

The call is inserted immediately after the `L_801BECC8` label, the mover's
epilogue, so it runs on **both** of the mover's paths: the default one, and the
"pull the cursor back towards the screen centre" branch at 0x801BE890
(`x = (2x + 160) / 3`, `y = (2y + 120) / 3`), which jumps straight to that label.
Running on both is what lets the app see a cursor the game moved itself: on the
centring path the hook adopts the game's position instead of overwriting it, so
the cursor stays on the world place the player acted on. The hook reads the state
pointer out of the module's own data word itself, so it does not depend on which
register holds the state on either path.

Bank units are outside the mod hook map (a `.nrm` hook resolves only through the
base ELF's sections), so this insertion is the only place to hook it. It is a
code *generation* fix, like `tools/map_cursor.py`: `make bank-recomp` regenerates
`BankNFuncs/`, so `Makefile` re-applies it on every run.

Usage:
    python3 tools/mission_cursor.py           # apply (idempotent)
    python3 tools/mission_cursor.py --revert  # remove
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UNIT = ROOT / "BankNFuncs"

# The first instruction of the mover's epilogue, which both of its paths reach.
# The address comment and the label occur once in the generated unit.
ANCHOR = (
    "L_801BECC8:\n"
    "    // 0x801BECC8: lw          $ra, 0x3C($sp)\n"
)

# The point on the mover's default path where its per-frame delta bytes still hold
# this call's values: the mover zeroes them at 0x801BEC48-0x801BEC58, so by the
# epilogue the evidence that the game moved the cursor itself is gone.
NOTE_ANCHOR = (
    "L_801BEC04:\n"
    "    // 0x801BEC04: lui         $v1, 0x801B\n"
)

NOTE_MARKER = "// mission cursor: record whether the mover moved the cursor itself."

NOTE = (
    "L_801BEC04:\n"
    "    " + NOTE_MARKER + "\n"
    "    ogre_mission_cursor_note_move(rdram);\n"
    "    // 0x801BEC04: lui         $v1, 0x801B\n"
)

NOTE_APPLIED = (
    "L_801BEC04:\n"
    "    " + NOTE_MARKER + "\n"
    "    ogre_mission_cursor_note_move(rdram);\n"
)

CALL_MARKER = "// mission cursor: place the cursor at the pointer (app/src/mission_cursor.cpp)."

CALL = (
    "L_801BECC8:\n"
    "    " + CALL_MARKER + "\n"
    "    // The hook reads `*(u32*)0x801AB870`, the mission field's state object.\n"
    "    ogre_mission_cursor(rdram);\n"
    "    // 0x801BECC8: lw          $ra, 0x3C($sp)\n"
)

DECL_MARKER = "// mission cursor: the app drives the mission field's cursor (scene 0x03)."

# `#include "funcs.h"` occurs once in the generated unit (line 2).
DECL_ANCHOR = '#include "funcs.h"\n'

DECL = (
    DECL_ANCHOR
    + "\n"
    + DECL_MARKER
    + "\n"
    + "#ifdef __cplusplus\n"
    + 'extern "C" void ogre_mission_cursor(uint8_t* rdram);\n'
    + 'extern "C" void ogre_mission_cursor_note_move(uint8_t* rdram);\n'
    + "#else\n"
    + "void ogre_mission_cursor(uint8_t* rdram);\n"
    + "void ogre_mission_cursor_note_move(uint8_t* rdram);\n"
    + "#endif\n"
)

APPLIED = (
    "L_801BECC8:\n"
    "    " + CALL_MARKER + "\n"
    "    // The hook reads `*(u32*)0x801AB870`, the mission field's state object.\n"
    "    ogre_mission_cursor(rdram);\n"
)

DECL_APPLIED = (
    "\n"
    + DECL_MARKER
    + "\n#ifdef __cplusplus\nextern \"C\" void ogre_mission_cursor(uint8_t* rdram, int32_t state);\n"
    + "#else\nvoid ogre_mission_cursor(uint8_t* rdram, int32_t state);\n#endif\n"
)


def target() -> Path:
    """The generated unit that defines `func_ovlN_801BE4E0`."""
    needle = "RECOMP_FUNC void func_ovlN_801BE4E0("
    hits = [p for p in sorted(UNIT.glob("funcs_*.c")) if needle in p.read_text()]
    if len(hits) != 1:
        print(
            f"mission_cursor: {len(hits)} generated unit(s) define func_ovlN_801BE4E0; "
            "expected exactly one. Run `make bank-recomp` first.",
            file=sys.stderr,
        )
        raise SystemExit(1)
    return hits[0]


def apply() -> int:
    path = target()
    src = path.read_text()
    if CALL_MARKER in src or NOTE_MARKER in src:
        print("mission_cursor: already applied")
        return 0
    for label, anchor in (("L_801BECC8", ANCHOR), ("L_801BEC04", NOTE_ANCHOR)):
        count = src.count(anchor)
        if count != 1:
            print(
                f"mission_cursor: the anchor {label} occurs {count} times in "
                f"{path.relative_to(ROOT)}; the generated mission module changed. Update this "
                "script rather than editing the generated C.",
                file=sys.stderr,
            )
            return 1
    if src.count(DECL_ANCHOR) != 1:
        print(
            f"mission_cursor: the declaration anchor occurs {src.count(DECL_ANCHOR)} times in "
            f"{path.relative_to(ROOT)}.",
            file=sys.stderr,
        )
        return 1
    src = src.replace(DECL_ANCHOR, DECL, 1)
    src = src.replace(ANCHOR, CALL, 1)
    src = src.replace(NOTE_ANCHOR, NOTE, 1)
    path.write_text(src)
    print(f"mission_cursor: applied to {path.relative_to(ROOT)}")
    return 0


def revert() -> int:
    path = target()
    src = path.read_text()
    if CALL_MARKER not in src and NOTE_MARKER not in src:
        print("mission_cursor: not applied")
        return 0
    src = src.replace(APPLIED, "L_801BECC8:\n", 1)
    src = src.replace(NOTE_APPLIED, "L_801BEC04:\n", 1)
    src = src.replace(DECL_APPLIED, "", 1)
    path.write_text(src)
    print(f"mission_cursor: reverted {path.relative_to(ROOT)}")
    return 0


def main() -> int:
    args = sys.argv[1:]
    if args == ["--revert"]:
        return revert()
    if args:
        print("usage: python3 tools/mission_cursor.py [--revert]", file=sys.stderr)
        return 2
    return apply()


if __name__ == "__main__":
    raise SystemExit(main())
