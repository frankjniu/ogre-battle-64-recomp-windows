#pragma once

#include <cstdint>

// The mission field's cursor, driven by the mouse (scene 0x03 only).
//
// The mission field keeps its cursor as two f32 SCREEN pixels in the state
// object at the fixed address 0x800E91D0. The mission's own modules hold that
// address as a pointer constant in their data (record 7's 0x801AB870, record
// 9's 0x80213690, record 2's 0x8019E1B4, record 3's 0x801AB870):
//
//   state[+0x04] (u8)   camera zoom level 0..2
//   state[+0x06] (s8)   per-frame cursor delta x
//   state[+0x07] (s8)   per-frame cursor delta y
//   state[+0x08] (f32)  cursor x, in framebuffer pixels (320x240, centre 160)
//   state[+0x0C] (f32)  cursor y, in framebuffer pixels (320x240, centre 120)
//   state[+0x10] (f32)  camera scroll position
//   state[+0x14] (f32)  camera scroll velocity
//   state[+0x18] (u8)   flags (bit 1 set by the mover at the end of every call)
//
// `func_ovlN_801BE4E0` (bank unit N, record 7) is the mover: it reads the held
// button word `*(0x800E8108)` (`D_800E79B0`), compares it against the D-pad
// masks of the profile at `*(0x801F0CC0)` (right +0x0E, left +0x0C, up +0x08,
// down +0x0A), integrates a per-frame delta and stores the pair at 0x801BE974
// (`swc1 $f4, 8(v1)`) / 0x801BE988 (`swc1 $f2, 12(v1)`), with `v1` from
// 0x801BE510 (`lw $v1, -18320($v1)`, i.e. `*(0x801AB870)`). It clamps the pair
// to x 40.0..280.0 (0x801BEB98/0x801BEBB8) and y 30.0..210.0
// (0x801BEBE0/0x801BEC00).
//
// `ogre_mission_cursor` is called at the generated label `L_801BECC8`, the
// mover's epilogue, so it runs on both of the mover's paths: the default one at
// `L_801BEC04` and the "pull the cursor back towards the screen centre" branch at
// `0x801BE890` (`x = (2x + 160) / 3`, `y = (2y + 120) / 3`), which jumps straight
// to the epilogue. The hook reads the state pointer out of the module's own data
// word (`*(u32*)0x801AB870`) and requires `0x800E91D0`, so it does not depend on
// which register happens to hold the state on either path. It is inserted into
// the generated bank code by `tools/mission_cursor.py` and re-applied by
// `make bank-recomp`; bank units are outside the mod hook map, so there is no
// other place to hook this (see `app/src/map_cursor.hpp` for the same pattern on
// the world map).
//
// The cursor is a servo the game runs continuously, not a free pointer: measured
// over a played run it moves 1-9 px (median 3.6) on nearly every frame with no
// button held (`none mode0`: 123 of 162 sampled frames), from the mover's own
// integration, its screen-centre pull, and the snap-to-unit writers
// (`func_ovlN_801B8070`, `func_ovlN_801B9694`). "The game moved it, so adopt"
// therefore locks the mouse out, and "the mouse always wins" discards the game's
// own recentring, which is what leaves the cursor off the place the player just
// acted on. The arbitration is by explicit signal instead:
//
//   * the D-pad or C-LEFT is held (`pad=`, `centre=`), or the mover integrated a
//     delta on this call (`nudge=`, sampled at `L_801BEC04` because the mover
//     zeroes those bytes before its epilogue), or
//   * the yield window after a click is still running (`yield=`). A click is the
//     one unambiguous signal that the game is about to move the cursor itself: a
//     command recentres the camera and carries the cursor with it, and the point
//     of the yield is that the cursor ends up on the place that was acted on, so
//     a second click confirms it.
//
// In every one of those cases the pointer adopts the game's position instead of
// overwriting it. Otherwise the pointer places the cursor.
extern "C" void ogre_mission_cursor(uint8_t* rdram);

// Called at the mover's `L_801BEC04`, which is on its default path only but is
// the one place where the per-frame delta bytes still hold this call's values.
// Records whether the mover moved the cursor itself; the placement above
// consumes and clears that. Without it the placement cannot tell a cursor the
// game moved from one the pointer moved, because the mover zeroes those bytes
// before its epilogue.
extern "C" void ogre_mission_cursor_note_move(uint8_t* rdram);

// The pointer moved over the game window. `window_x`/`window_y` are window
// points and the two size pairs are the window and its drawable; the hook
// aspect-fits the 320x240 framebuffer inside the drawable. Called from the
// platform thread (`pump_sdl_events`).
extern "C" void ogre_mission_cursor_note_mouse(int window_x, int window_y, int win_w, int win_h,
                                               int draw_w, int draw_h);

// The pointer left the game window, or the overlay owns it.
extern "C" void ogre_mission_cursor_clear();

// A left click on the picture: press the game's `A` for a few frames so the
// location under the cursor is confirmed. `A` is the field's confirm button; the
// press is OR'd into controller 0 by `get_input`, which is the same path the
// keyboard and the live console's `press` command use.
extern "C" void ogre_mission_cursor_click(int window_x, int window_y, int win_w, int win_h,
                                          int draw_w, int draw_h);

// Called from `get_input`. Returns true while the armed click's `A` is held.
// The armed count is consumed on every poll whatever screen is up, so a click
// that lands on another screen expires instead of waiting to press `A` on this
// one the next time it appears. One click arms both this and the world map's.
extern "C" bool ogre_mission_cursor_take_confirm();

// Called from `get_input`. The N64 D-pad bits for the direction the pointer is
// pushing past the field's play area, or 0.
//
// The field's camera pans with the cursor, not with a held direction: the mover
// `func_ovlN_801BE4E0` converts the held D-pad into the per-frame delta bytes
// `state+0x06`/`+0x07` (0x801BE5A8 `li $v1,26`, then the ramp at 0x801BE6D4),
// and the camera scroll `func_ovlN_801BECF0` (called right after the mover at
// 0x801AF130) reads those same bytes and adds `delta/128 * 8.5 * table[e]` to
// the eye x and the look-at x of the camera at `*(u32*)0x8019F5AC`
// (0x801BEE04-0x801BEE1C), clamped to the map's world rect at `0x801F0DD8`.
// It only runs while `*(0x801F3630)` is set, which the mover sets only when the
// cursor pair actually changed on that call (0x801BEC3C-0x801BEC70), so the
// pan is coupled to cursor movement. Holding the game's own D-pad is therefore
// how the pointer scrolls the field: the pointer's own pixel is clamped by the
// mover to 40..280 / 30..210, so a pointer past that bound cannot place the
// cursor there and the intent has to arrive as input. This is the same
// mechanism as the world map's edge scroll; see `app/src/map_cursor.hpp`.
extern "C" uint16_t ogre_mission_cursor_edge_direction();

// Environment:
//   OGRE_MISSION_CURSOR=0                  disable the placement and the click's A
//   OGRE_MISSION_CURSOR_LOG=1              one `[missioncursor]` line per change
//   OGRE_MISSION_CURSOR_CONFIRM_POLLS=<n>  how many polls a click holds A (8)
//   OGRE_MISSION_CURSOR_YIELD=<n>          frames after a click during which the
//                                          pointer adopts the game's cursor
//                                          instead of placing it (15, ~0.25 s)
//   OGRE_MISSION_CURSOR_AT=<x>,<y>         park the pointer at that framebuffer
//                                          pixel once, with no mouse event: a
//                                          measurement aid for scripted runs
//   OGRE_MISSION_CURSOR_EDGE=<n>           how close to a play-area bound the
//                                          pointer may be before it pushes that
//                                          direction (24 px)
//   OGRE_MISSION_CURSOR_EDGE_FORCE=<mask>  hold that raw D-pad mask (hex or
//                                          decimal) as if the pointer were at
//                                          that edge, with no mouse event: a
//                                          measurement aid for scripted runs
