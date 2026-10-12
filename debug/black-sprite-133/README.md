# session 133 — the black-head capture

Scratch readers and rendered evidence for `docs/HANDOFF-2026-10-11-session133.md`.
They read the developer's five presses in
`~/Desktop/ogre-battle-64-recomp/frame-capture/` (`ram-1-full.bin` head correct,
`ram-2..5` head black) and none of them writes to the tree.

| file | what |
|---|---|
| `loads.py` | list every `LOADTILE`/`LOADBLOCK` in a dump with the RDRAM it reads and its non-zero count |
| `crop.py` / `fb.py` | render the game's framebuffer (`0x80000400`, `0x80025C00`, `0x8004B400`) as a PNG |
| `rend2.py` / `parts.py` / `montage2.py` | render a texture or a contact sheet of the `36 34` asset records |
| `frame.py` / `dlwalk.py` / `compose.py` / `s2dex.py` | earlier display-list walkers; `tools/ogre_frame.py` supersedes them |
| `crop1.png` | the unit at the correct press (purple head) |
| `crop3.png`, `crop3-0x*.png` | the unit at a failing press, from all three framebuffers (flat `0x2947` head) |
| `fb3-0x80000400.png` | the whole rendered frame at a failing press |
| `alpha-good-colour.png`, `alpha-bad-colour.png` | the head's RGBA16 colour payloads, decoded (both are heads) |
| `frames/press1..5.png` | `tools/ogre_frame.py` output for the five presses (TEXRECT layer only) |

The exact command lines are in the handoff's §7 table.
