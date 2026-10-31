// Hang report (see hang_watch.cpp): writes error.log when the game stops
// producing frames while the window stays open.
#pragma once

#include <SDL.h>

namespace ogre {

// One check on the main thread; `update_gfx` calls it about once per
// millisecond. Reads two guest words and does nothing else until the game's
// frame counter has been still for the threshold. `game_window` is the window
// RT64 draws into; time while it is minimized or hidden is not counted.
void hang_watch_tick(SDL_Window* game_window);

}  // namespace ogre
