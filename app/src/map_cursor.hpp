#pragma once

#include <cstdint>

// The world map's cursor, driven by the mouse (scene 0x05 only), and only while
// the MOUSE ENABLED setting is on (`settings.cpp`, off by default;
// `OGRE_MAP_CURSOR=1`/`=0` overrides it for one run).
//
// Milestone A (landed, developer-confirmed 2026-10-10): the cursor sprite
// follows the pointer smoothly. The game clamps the result to its play area
// (x >= 0x28, the state machine's own clamp), so a pointer outside the map
// holds the cursor on the frame.
//
// The map module records the cursor's position as screen pixels in its state
// object, `state = *(u32*)0x80197B18`:
//
//   state[+0x50] (u8)   1 = draw the cursor
//   state[+0x52] (s16)  cursor x, clamped by the state machine to 0x28..0x110
//   state[+0x54] (s16)  cursor y, clamped by the state machine to 0x28..0xC0
//
// and the scene hook's cursor draw reads +0x52/+0x54 and emits the sprite there
// (`func_ovlM_801A103C` at 0x801A1C48-0x801A1C50, ROM 0x801A1C48; the state
// machine that keeps the pair up to date is `func_ovlM_8019AF0C`).
//
// `ogre_map_cursor` is called from that draw, immediately before it reads the
// pair, so a write here reaches the frame in progress. It is inserted into the
// generated bank code by `tools/map_cursor.py` and re-applied by
// `make bank-recomp`; bank units are outside the mod hook map, so there is no
// other place to hook this (see `app/src/hd_backgrounds.cpp` for the same
// pattern).
extern "C" void ogre_map_cursor(uint8_t* rdram, int32_t state);

// The pointer moved over the game window. `window_x`/`window_y` are window
// points and the two size pairs are the window and its drawable; the hook
// aspect-fits the 320x240 framebuffer inside the drawable. Called from the
// platform thread (`pump_sdl_events`).
extern "C" void ogre_map_cursor_note_mouse(int window_x, int window_y, int win_w, int win_h,
                                           int draw_w, int draw_h);

// The pointer left the game window, or the overlay owns it.
extern "C" void ogre_map_cursor_clear();

// The MOUSE ENABLED setting, published by `settings.cpp` at boot and whenever
// the row changes. Off by default, so the pointer drives nothing until the
// player turns it on. `OGRE_MAP_CURSOR` overrides it for one run: `1`/`on`
// forces it on, `0`/`off` forces it off.
extern "C" void ogre_map_cursor_set_enabled(bool on);

// A left click on the picture: press the game's `A` for a few frames so the
// location under the cursor is selected. `A` is the map's confirm button; the
// press is OR'd into controller 0 by `get_input`, which is the same path the
// keyboard and the live console's `press` command use. A click in the pillarbox
// arms nothing.
extern "C" void ogre_map_cursor_click(int window_x, int window_y, int win_w, int win_h,
                                      int draw_w, int draw_h);

// Called from `get_input`. Returns true while the armed click's `A` is held.
// The armed count is consumed on every poll whatever screen is up, so a click
// that lands on another screen expires instead of waiting to press `A` on this
// one the next time it appears. See `app/src/mission_cursor.hpp`.
extern "C" bool ogre_map_cursor_take_confirm();

// Called from `get_input`. The N64 D-pad bits for the direction the pointer is
// pushing past the map's edge, or 0. The map scrolls its view while a direction
// is held, so this is how the pointer scrolls the map: the pointer's own pixel
// is clamped by the game, so the intent has to arrive as input.
extern "C" uint16_t ogre_map_cursor_edge_direction();
