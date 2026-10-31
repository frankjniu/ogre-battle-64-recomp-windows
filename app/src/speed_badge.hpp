// The game-speed badge: "2x", "4x", "8x" in the game window's lower-right
// corner for a couple of seconds after the multiplier changes (see
// speed_badge.cpp).
//
// The picture is drawn by RT64 straight to its own swap chain (Metal/Vulkan/
// D3D12) and the renderer has no UI pass, so the badge is a second borderless,
// always-on-top SDL window placed over the corner of the game window, drawn
// with the same bitmap font as the launcher and the Esc overlay. That is the
// technique overlay.cpp already uses for the panel.
#pragma once

#include <SDL.h>

namespace ogre {

// Remembers the game window the badge is positioned over. Call once, after
// `create_window` and before the first frame. No window is created here: the
// badge's own window appears the first time the speed changes.
void speed_badge_init(SDL_Window* game_window);

// One frame on the main thread. Applies the scripted speed inputs
// (`OGRE_SPEED_KEYS`, `OGRE_SPEED_PAD`), notices a changed multiplier and shows
// the badge for `kSpeedBadgeHoldMs` (see speed_badge.cpp), and draws and hides
// it as that window passes.
void speed_badge_tick();

// Shows `text` in the badge for the badge's hold time, replacing whatever it
// shows (quick save uses it). Not drawn in fullscreen, like the speed readout.
void speed_badge_message(const char* text);

// Destroys the badge window and renderer.
void speed_badge_shutdown();

// The badge's own SDL window, or nullptr before it exists. The platform treats
// a pointer over it as a pointer over the game window, because SDL2's window
// hit test has no click-through result (`SDL_HitTestResult` is NORMAL,
// DRAGGABLE and the resize edges) and the badge window would otherwise take the
// mouse focus while the pointer is over it.
SDL_Window* speed_badge_window();

}  // namespace ogre
