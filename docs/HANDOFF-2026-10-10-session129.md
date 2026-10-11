# Handoff — 2026-10-10, session 129: the mission field's view scrolls at the pointer's edge

Developer request: *"read the latest handoff, and let's continue the mouse support
feature in the mission screen. this time we need to make the camera scroll when
the cursor is close to the edges."*

**Result: the mission field's camera scrolls while the pointer is close to a
play-area edge.** The pointer holds the direction the field's own mover reads,
and the game's own camera pan does the scrolling. Developer-confirmed twice:
*"I could move the camera around by moving the cursor close to the edges"*. No
other screen's cursor changed.

## 1. What the field's camera scroll is, at instruction level

The camera pans with the **cursor**, and not with a held direction. Three
functions, in call order inside the field's frame update
(`func_ovlN_801AE880`):

| address | instruction | what |
|---|---|---|
| `0x801AF0DC` | `jal 801be4e0` | `func_ovlN_801BE4E0`, the cursor mover, with `f12 = 32.0` |
| `0x801AF130` | `jal 801becf0` | `func_ovlN_801BECF0`, the camera pan, with `f12 = 8.5 * table[e]` |
| `0x801AF138` | `jal 801bef74` | `func_ovlN_801BEF74`, the zoom/L-R handler |

The mover turns the held D-pad into the per-frame delta bytes `state+0x06`
(x) and `state+0x07` (y): `0x801BE588` reads profile `+0x0E` and `0x801BE5A8`
loads `li $v1,26`; the ramp at `0x801BE6D4` computes `5*counter` (counter
saturates at 10, `0x801BE688`); `0x801BE924`-`0x801BE988` integrates
`delta/128 * 32.0` into the cursor pair.

`func_ovlN_801BECF0` reads the same two bytes and moves the camera:

| address | instruction | what |
|---|---|---|
| `0x801BED3C`/`0x801BED58` | `lb $v0,6($v1)` / `lb $v0,7($v1)` | the delta bytes, from `state = *(0x801AB870)` |
| `0x801BED54`/`0x801BED6C` | `div.s` by `0x4300` | `/128` |
| `0x801BED70`/`0x801BED7C` | `mul.s $f20,$f2,$f22` | `* f12` (the caller's `8.5 * table[e]`) |
| `0x801BED84`-`0x801BED98` | `func_ovlN_801AAE30` | cursor screen pixel → world point, into `sp+24`/`sp+32` |
| `0x801BEDD0`-`0x801BEE5C` | `lwc1 ... 0x801F0DD8` … | clamp `world + delta` to the world rect |
| `0x801BEE04`-`0x801BEE1C` | `lw $v0,-2644($v0)` / `swc1` `+0x20`, `+0x14` | camera x: eye and look-at |
| `0x801BEE9C`-`0x801BEF5C` | same for `+0x1C` and `+0x28` | camera z |

The pan is gated by `*(0x801F3630)`, which the mover sets only when the cursor
pair changed on that call (`0x801BEC3C`: `andi $v0,$s2,0xff`; `0x801BEC40`:
`bnez`; `0x801BEC6C`: `sw $v0,13872($at)`), and the mover zeroes the delta bytes
when the pair did not change (`0x801BEC48`-`0x801BEC58`). The pan is therefore
coupled to cursor movement: it follows the cursor, and it stops when the cursor
stops.

**The camera pointer.** `lw $v0,-2644($v0)` is preceded by `lui $v0,0x8019`, so
the word is `0x80190000 - 0xA54 = 0x8018F5AC` (in `streamedB`), and not
`0x8019F5AC`. The struct:

| offset | what |
|---|---|
| `+0x00` | field of view (used by `func_ovlA_801AAE30`) |
| `+0x14` | eye position x (`+0x18` y, `+0x1C` z) |
| `+0x20` | look-at point x (`+0x24` y, `+0x28` z) |
| `+0x2C` | up vector x (`+0x30` y, `+0x34` z) |

`func_ovlA_801AAE30` (bank unit A, record 3) subtracts `+0x20 - +0x14`, crosses
with `+0x2C`, normalises, and intersects the ground plane at `y = 0`
(`0x801AAFB0`: `t = -cam[0x18] / dir.y`, then `world = eye + t*dir`). So `+0x14`
is the eye and `+0x18` its height above the ground. `func_ovlN_801BECF0` adds
the same delta to the eye and the look-at, which is a translation and not a
rotation.

**Why the pointer's own pixel cannot do this.** The mover clamps the pair to
x 40.0..280.0 (`0x801BEB98`/`0x801BEBB8`) and y 30.0..210.0
(`0x801BEBE0`/`0x801BEC00`). A pointer past that bound names a pixel the mover
will not store, and writing it from the hook does not move the camera, because a
pair that does not change leaves `*(0x801F3630)` clear. The intent has to arrive
as input, which is the world map's mechanism (`app/src/map_cursor.hpp`).

## 2. The change

- **`app/src/mission_cursor.cpp`** — `edge_direction_for(fb_x, fb_y)` returns the
  N64 D-pad bits for a pointer within `OGRE_MISSION_CURSOR_EDGE` (default 24)
  pixels of a play-area bound (40/280 x, 30/210 y, the mover's own clamps);
  `ogre_mission_cursor_note_mouse` samples it beside the pointer pixel, and
  `ogre_mission_cursor_clear` clears it. `ogre_mission_cursor_edge_direction()`
  returns it while the field's mover is running (`mission_is_active()`, the same
  gate as the click's `A`), or the value of the test aid
  `OGRE_MISSION_CURSOR_EDGE_FORCE`. The `[missioncursor]` log line gained the
  camera and `edge=`.
- **`app/src/mission_cursor.hpp`** — the declaration and the instruction-level
  note above.
- **`app/src/sdl_platform.cpp`** — `get_input` ORs
  `ogre_mission_cursor_edge_direction()` into controller 0 beside the map's.
- **Docs** — `docs/guides/app-build.md` §"The mission field's mouse cursor",
  `docs/DECISIONS.md`, `PLAN.md`, `docs/STATUS-LOG.md`, `docs/scenes.md`,
  `docs/README.md`.

**The mover's profile uses the N64 D-pad bits.** The mover compares the held
word against `*(0x801F0CC0)` — a per-mode profile — so the injected bits must be
that profile's masks. The one-time log line now prints them, and one scripted run
gives `right=0x0100 left=0x0200 up=0x0800 down=0x0400`, the standard N64 bits.
That is why the same masks the world map injects work here.

**The arbitration needed no change.** While a direction is held the hook already
adopts the game's cursor (the `pad=` signal, session 128), and the injected bit
appears in the same held word the hook reads, so the push and the pointer cannot
fight, and the release still has no jump.

## 3. Two defects in session 128's log line

`[missioncursor]` had 15 `%` conversions for 13 arguments: the format carried a
stray `(%.2f,%.2f)` after `mouse=`, and gcc reports
`-Wformat-insufficient-args`. The surviving session-128 logs show the effect:
the extra pair consumes the last two float arguments, so the trailing
`-> (%.2f,%.2f)` prints `(0.00,0.00)` (`/tmp/mc-play.log`, `/tmp/mc-play5.log`).
The integer fields are unaffected, because on the x86-64 and AArch64 varargs
ABIs the integer and floating-point arguments are consumed from separate save
areas. The stray pair is removed here.

The camera was first read at `0x8019F5AC`, which printed `cam=0.00,...` in a run
that held a direction for 946 frames. The instruction is `lw $v0,-2644($v0)`
with `lui $v0,0x8019`; `-2644 = -0xA54`, so the word is at `0x8018F5AC`.

## 4. What was run for verification

The mission was reached with the developer's battery save and session 112's tap
recipe, in its own config dir, so nothing wrote back to the save file:

```
OGRE_SAVE=assets/scene_5_suspend.bin OGRE_SAVE_RESET=1 OGRE_PREF_DIR=<fresh dir> \
OGRE_TAP_MS=2000 OGRE_TAP_SCENE_BUTTON="title:start:1,0x12:a:1,0x03:a:4" \
OGRE_LIVE_CONSOLE=1 OGRE_CONSOLE_ON_SCENE=0x03 OGRE_CONSOLE_ON_CMD='press right 900' \
OGRE_MISSION_CURSOR_LOG=1 OGRE_SPEED=4 OGRE_EXIT_AFTER_MS=100000 ./build-app/ogrebattle64
```

| run | evidence |
|---|---|
| `/tmp/mc-edge.log` | The first measurement, with the camera read at the wrong address: 946 `pad=1` frames, `cam=0.00,...` in every line. That is what exposed the address error. |
| `/tmp/mc-verify2.log` | `hook active on the mission field (profile right=0x0100 left=0x0200 up=0x0800 down=0x0400)`; 1790 `[missioncursor]` lines; the camera moves through the hold (`cam=-6.92,3.17/-6.92,1.11` → `2.10,...` → `-7.50,...` at the world clamp) while the cursor is clamped at 40/280 and 30/210; exit 0; `tools/runlog.py --check` PASS (`no crash, no stub call, no unknown module, no bad RSP exit, no stubbed non-gfx task`). |
| developer rounds | Two played rounds with a real mouse. The camera could be moved around by moving the cursor close to the edges. The pointer was hard to control at the increased game speed and easy at normal speed; that is the game speed and not the feature, and the edge push itself was not reported as a problem. |

Builds: `cmake --build build-app --target ogrebattle64` and the same for
`build-null` both link with no warning from the two files that changed.

**Not verified:** the on-screen pan was not measured in pixels, and the pan's
speed is whatever the game's own ramp gives (`delta/128 * 8.5 * table[e]`, with
`table[e] = 0.1` for the entries read from ROM `0x65734`). The zoom level the
input was taken at is in the log as `mode=` (the field mode at
`*(0x801F0E00)`), and not the camera's zoom at `state+0x04`.

## 5. Files changed

| File | What |
|---|---|
| `app/src/mission_cursor.cpp` | the edge direction, the camera read, the log fixes |
| `app/src/mission_cursor.hpp` | the declaration and the evidence |
| `app/src/sdl_platform.cpp` | the mission edge direction is OR'd into controller 0 |
| `docs/guides/app-build.md` | the mechanism and the two new knobs |
| `docs/DECISIONS.md`, `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md` | the record |
| `docs/scenes.md` | the mission field's cursor row |
| `docs/HANDOFF-2026-10-10-session129.md` | this file |

No generated bank code changed: the two hook sites `tools/mission_cursor.py`
owns are unchanged.

## 6. Probes used and reverted

None. The camera read is part of the diagnostic log
(`OGRE_MISSION_CURSOR_LOG=1`), not a temporary probe, because the feature is
about the camera. No `RecompiledFuncs/` or `Bank*Funcs/` file was touched.

## 7. Knobs

| variable | what |
|---|---|
| `OGRE_MISSION_CURSOR_EDGE=<n>` | how close to a play-area bound the pointer may be before it holds that direction (default 24 px) |
| `OGRE_MISSION_CURSOR_EDGE_FORCE=<mask>` | hold that raw D-pad mask (hex or decimal, e.g. `0x100`) with no mouse event: a measurement aid for scripted runs |
| `OGRE_MISSION_CURSOR=0` | disables the placement, the click's `A` and the edge push |

## 8. Open

- **The camera stalls when the cursor stops moving, and that is the game's own
  coupling, not a defect here.** `func_ovlN_801BECF0` runs only while the mover
  set `*(0x801F3630)`, and the mover sets it only when the cursor pair changed on
  that call. The scripted run shows the pan reaching the world rect
  (`0x801F0DD8`) and stopping there, which is the game's own clamp.
- **The edge margin is a first guess (24 px), from the world map's value.** The
  field's play area is 240x180 px against the map's 232x152, so the proportions
  are close. If the push starts too early or too late, `OGRE_MISSION_CURSOR_EDGE`
  changes it without a rebuild.
- **The field's `R` hold-menu, the unit-command menu and the Organize Screen
  (scene `0x06`, bank unit S) are still not mouse-driven** (session 128's open
  item).
