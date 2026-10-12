// OGRE_RAM_KEY: press a digit key and the run writes a file with the guest
// RDRAM at a chosen address, so a bug can be investigated offline without the
// developer copying anything out of a terminal.
//
// Session 130 uses it for a palette table the battle scene builds: one slot of
// 24 is all zero, and the run only shows it as a black character part. The dump
// answers "is this slot zero in the game" and "what do the other slots hold"
// in one key press, with no timing requirement (the table is filled when the
// scene starts and stays put).
//
// Knobs:
//   OGRE_RAM_KEY=<1..9>       the key that takes the dump
//   OGRE_RAM_KEY_ADDR=<hex>   the guest address to dump (default 0x802696C8,
//                             the battle part-palette table)
//   OGRE_RAM_KEY_LEN=<n>      bytes to dump (default 0x1000)
//   OGRE_RAM_KEY_OUT=<dir>    where to write (default the run's config dir)
//   OGRE_RAM_KEY_FULL=1       write the whole 8 MiB image instead of the window,
//                             to `<dir>/ram-<n>-full.bin`. Session 130 could
//                             not find the buffer that holds one black sprite
//                             part from a window alone: with the whole image the
//                             address the draw binds can be searched for as a
//                             pointer, which names the table and the buffer.
//
// The output is `<dir>/ram-<n>.bin` (the bytes) and `<dir>/ram-<n>.txt` (the
// same bytes as hex per 16, plus the 0x80-byte slots spelled out, which is what
// makes an all-zero slot visible at a glance). **Every press takes another
// dump**, numbered in sequence, so one run can compare a buffer before and
// after an animation step (session 131: the sprite part that goes black when a
// character bows its head).
#pragma once

#include <cstdint>

namespace ogre {

// Call once per frame from the platform update, after SDL's keyboard state is
// current. Reads the environment on the first call; does nothing when
// OGRE_RAM_KEY is unset.
void ram_key_tick(uint8_t* rdram);

// Write the whole 8 MiB image to `<OGRE_PREF_DIR>/ram-<serial>-full.bin`.
//
// This exists so a fault can be captured without a human key press: the
// renderer's texture trace calls it the first time it sees a draw paint black
// (session 132). A key press cannot bracket a per-pose fault -- every capture
// taken that way missed the frame -- and the trace has already decided the draw
// is black, which is exactly the moment the image is worth having. `serial` is
// the caller's sequence number, so repeated calls do not overwrite.
//
// Returns true when the file was written. Safe to call while the game thread is
// parked on the RSP task (the renderer's send_dl path), which is where the trace
// runs.
bool ram_key_dump_full(uint8_t* rdram, int serial);

}  // namespace ogre
