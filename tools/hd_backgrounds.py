#!/usr/bin/env python3
"""Call the app's HD-background hooks from every njpeg pipeline.

The game decodes its full-screen njpeg images (`docs/guides/njpeg-backgrounds.md`)
with one module that four records carry, each with its own state object:

| copy | unit | state pointer | decode setup | readback |
|---|---|---|---|---|
| dialogue engine (scene 0x02/0x0D) | bank E, record 0 | `0x8019A680` | `func_ovlE_80199A08` | `func_ovlE_8019976C` |
| battle and movie (record 10) | bank C, `bankRec10` | `0x801D0700` | `func_ovlC_801CE084` | `func_ovlC_801CDDE8` |
| record 10b | bank C, `bankRec10b` | `0x801F0570` | `func_ovlC_801EF4A4` | `func_ovlC_801EF208` |
| title module (streamedC) | `RecompiledFuncs` | `0x801BA6F0` | `func_801B7874` | `func_801B75D8` |

The four copies are the same code at different addresses: the decode setup
loads the compressed stream's address from `state+0x6C` into `$a0` and calls
the CPU Huffman decoder `func_8008B250`; the readback copies one pass of pixels
and ends with `sb $v0, 0x7E($v1)` (the pass counter) before its return label.

Bank units are outside the `.nrm` mod hook map (`docs/guides/app-build.md` ->
"What a hook can reach"), so the app is called from the generated code. Each
pipeline gets two calls (`app/src/hd_backgrounds.cpp`):

    ogre_njpeg_source(rdram, STATE);   before the call to func_8008B250:
                                       records which asset this pass decodes
    ogre_njpeg_pass(rdram, STATE);     after the pass-counter store: lets the
                                       pack replace the pass's pixels

Both are defined for every build and are no-ops without a pack.

It is a code *generation* fix, like `tools/njpeg_readback.py`: `make
bank-recomp` and `make recomp` regenerate the files, and both run this script.
A file that is not present is skipped, so the script works after either target.

Usage:
    python3 tools/hd_backgrounds.py           # apply (idempotent)
    python3 tools/hd_backgrounds.py --revert  # remove
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

MARK = "// hd backgrounds"

# (label, file, state pointer, Huffman call address, pass-end store address)
SITES = [
    ("dialogue", "BankEFuncs/funcs_0.c", 0x8019A680, 0x80199CCC, 0x80199950),
    ("record 10", "BankCFuncs/funcs_2.c", 0x801D0700, 0x801CE348, None),
    ("record 10", "BankCFuncs/funcs_5.c", 0x801D0700, None, 0x801CDFCC),
    ("record 10b", "BankCFuncs/funcs_5.c", 0x801F0570, 0x801EF768, None),
    ("record 10b", "BankCFuncs/funcs_10.c", 0x801F0570, None, 0x801EF3EC),
    ("title", "RecompiledFuncs/funcs_14.c", 0x801BA6F0, 0x801B7B38, 0x801B77BC),
]

DECL = (
    MARK + ": njpeg source and pass hooks (app/src/hd_backgrounds.cpp).\n"
    "#ifdef __cplusplus\n"
    'extern "C" {\n'
    "#endif\n"
    "void ogre_njpeg_source(uint8_t* rdram, uint32_t state_ptr);\n"
    "void ogre_njpeg_pass(uint8_t* rdram, uint32_t state_ptr);\n"
    "#ifdef __cplusplus\n"
    "}\n"
    "#endif\n"
)

# The previous version of this script inserted one call in bank E.
LEGACY_CALL_RE = re.compile(
    r"\n    // hd backgrounds: the pass's pixels are in their buffer now\..*?"
    r"\n    ogre_hd_background\(rdram\);\n",
    re.DOTALL,
)
LEGACY_DECL_RE = re.compile(
    r"\n// hd backgrounds: replace a copied backdrop with a pack image\.\n"
    r"#ifdef __cplusplus\nextern \"C\" void ogre_hd_background\(uint8_t\* rdram\);\n"
    r"#else\nvoid ogre_hd_background\(uint8_t\* rdram\);\n#endif\n",
)

DECL_RE = re.compile(r"\n" + re.escape(MARK) + r": njpeg source and pass hooks.*?#endif\n.*?#endif\n",
                     re.DOTALL)
CALL_RE = re.compile(r"    " + re.escape(MARK) + r": [^\n]*\n    ogre_njpeg_(source|pass)\(rdram, 0x[0-9A-F]+u\);\n")


def source_anchor(addr: int) -> re.Pattern:
    # The jal's comment, its delay slot (emitted first) and the call itself.
    return re.compile(
        rf"(    // 0x{addr:08X}: jal         0x8008B250\n"
        rf"    // 0x{addr + 4:08X}: lw          \$a1, 0x68\(\$v0\)\n"
        rf"    ctx->r5 = MEM_W\(ctx->r2, 0X68\);\n)"
        rf"(    (?:LOOKUP_FUNC\(0x8008B250\)|func_8008B250)\(rdram, ctx\);\n)"
    )


def pass_anchor(addr: int) -> re.Pattern:
    return re.compile(
        rf"(    // 0x{addr:08X}: sb          \$v0, 0x7E\(\$v1\)\n"
        rf"    MEM_B\(0X7E, ctx->r3\) = ctx->r2;\n)"
        rf"(L_{addr + 4:08X}:\n)"
    )


def apply() -> int:
    by_file: dict[str, list] = {}
    for site in SITES:
        by_file.setdefault(site[1], []).append(site)
    status = 0
    for rel, sites in by_file.items():
        path = ROOT / rel
        if not path.exists():
            print(f"hd_backgrounds: {rel} not generated; skipped")
            continue
        src = path.read_bytes().decode()
        src, _ = LEGACY_CALL_RE.subn("\n", src)
        src, _ = LEGACY_DECL_RE.subn("\n", src)
        changed = False
        for label, _rel, state, src_addr, pass_addr in sites:
            for addr, anchor, fn in ((src_addr, source_anchor, "source"),
                                     (pass_addr, pass_anchor, "pass")):
                if addr is None:
                    continue
                call = (f"    {MARK}: {label} njpeg {fn} (0x{addr:08X})\n"
                        f"    ogre_njpeg_{fn}(rdram, 0x{state:08X}u);\n")
                if call in src:
                    continue
                pat = anchor(addr)
                if len(pat.findall(src)) != 1:
                    print(f"hd_backgrounds: error: no unique anchor for the {label} {fn} hook "
                          f"at 0x{addr:08X} in {rel}; did the recompiler output change?",
                          file=sys.stderr)
                    status = 1
                    continue
                src = pat.sub(lambda m: m.group(1) + call + m.group(2), src, count=1)
                changed = True
        if MARK + ": njpeg source and pass hooks" not in src:
            header = '#include "recomp.h"\n'
            if header not in src:
                print(f"hd_backgrounds: error: no {header!r} in {rel}", file=sys.stderr)
                return 1
            src = src.replace(header, header + DECL, 1)
            changed = True
        if changed:
            path.write_bytes(src.encode())
            print(f"hd_backgrounds: patched {rel}")
        else:
            print(f"hd_backgrounds: {rel} already applied")
    return status


def revert() -> int:
    for rel in sorted({s[1] for s in SITES}):
        path = ROOT / rel
        if not path.exists():
            continue
        src = path.read_bytes().decode()
        src, n1 = CALL_RE.subn("", src)
        src, n2 = DECL_RE.subn("\n", src)
        src, n3 = LEGACY_CALL_RE.subn("\n", src)
        src, n4 = LEGACY_DECL_RE.subn("\n", src)
        if n1 + n2 + n3 + n4:
            path.write_bytes(src.encode())
            print(f"hd_backgrounds: reverted {n1 + n2 + n3 + n4} block(s) in {rel}")
    return 0


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--revert":
        return revert()
    if len(sys.argv) != 1:
        print("usage: hd_backgrounds.py [--revert]", file=sys.stderr)
        return 2
    return apply()


if __name__ == "__main__":
    sys.exit(main())
