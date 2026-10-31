// Quick save (F5) and quick load (F9): a quick slot of the game's battery save.
//
// The port cannot snapshot the running machine for the player. The game's
// threads are native threads whose call stacks are not in N64 RAM, so a RAM
// image is only safe to restore while every thread is at the same place in the
// code as when it was taken (the console's debug checkpoints, app-build.md ->
// "Checkpoints"). The quick slot therefore holds the battery save: what the game
// last wrote at one of its own save points.
//
// * F5 copies the battery file (`<config>/saves/<game id>.bin`, written by the
//   runtime's saving thread a few ms after the game's last write) to
//   `<config>/saves/<game id>-quick.bin`.
// * F9 asks for confirmation (a second F9 within 3 s), puts the quick slot back
//   as the battery through the runtime's own save buffer (`save_write_ptr`; the
//   saving thread rewrites the battery file and keeps the previous one as
//   `.bak`), waits for the file, and restarts the program. The new process boots
//   to the title with the restored battery, and the player chooses Continue.
//
// A restart, and not a jump to the title inside the running game: storing the
// title request `0x8004` from the cutscene scene `0x0D` entered the title, and
// then the frame pump (N64 thread 4) called the cutscene engine's code after the
// title's bank had replaced it (`[lookup]` lines, then an access violation). The
// game never goes from `0x0D` to the title itself, so that scene's per-frame
// callbacks are never torn down on that path.
//
// Both keys act only while the game window exists and the Esc panel is closed.
// The speed badge window shows the result (not in fullscreen, where RT64 owns
// the display); every action also logs a `[quicksave]` line.
#pragma once

#include <SDL.h>

namespace ogre {

// Handles F5 and F9. Returns true when the key was one of them. Called from the
// event pump in sdl_platform.cpp with the same conditions as the speed keys.
bool quick_save_handle_key(SDL_Keycode key);

// One frame on the main thread: expires an unconfirmed F9 and delivers
// `OGRE_QUICK_KEYS` (scripted presses for a test run: `<ms>:<f5|f9>,...`,
// milliseconds after the first frame).
void quick_save_tick();

// True after a quick load restored the battery; the frame callback then ends
// the run through its normal exit path.
bool quick_save_restart_requested();

// Starts this executable again with the same command line, with
// `OGRE_RELAUNCH=1` so it boots past the start screen (main.cpp). Called at the
// end of main, after the runtime and SDL are shut down. Windows only; elsewhere
// it logs that the player should start the game again.
void quick_save_relaunch();

}  // namespace ogre
