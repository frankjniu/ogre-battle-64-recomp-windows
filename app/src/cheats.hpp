// Cheats from the CHEATS tab of the launcher and the Esc overlay.
//
// Each cheat is a toggle in settings.hpp (`cheat_max_goth`, `cheat_max_items`)
// and, while ON, is held every frame like a GameShark constant write: the game
// may spend goth or use an item, and the next frame restores the value.
// Turning a cheat OFF stops the writes and leaves the current values. Saving
// while a cheat is ON stores the cheated values in the battery save.
//
// The addresses are this ROM's (USA Rev A), read from its code; the published
// GameShark codes target another revision and sit 0x20 bytes lower in this
// block (docs/symbols.md -> goth, item lists).
//
// * MAX GOTH: goth, a u32 at `0x80196A8C`, held at 9,999,999, the game's own
//   cap. The shop compares it with the price (`lw` at `0x8021564C`, bank AB)
//   and the battle reward adds to it and clamps at `0x0098967F` (bank N,
//   `0x801E53B0`).
// * MAX ITEM COUNTS: every non-empty slot of the two owned-item lists held at
//   99, the game's cap. The lists are the 278-slot `0x80196B20` and the 40-slot
//   `0x80193AE0`, 4 bytes a slot: `+0x00` u16 item id (0 = empty), `+0x03` u8
//   count. The game's own debug cheat (`func_ovlS_801C214C`, `0x801C25B0`) walks
//   the same two lists with the same empty test and sets 98.
#pragma once

namespace ogre {

// Applies the cheats that are ON to the running game's RDRAM. Called once per
// frame on the main thread, from the frame callback in main.cpp. A no-op while
// the runtime has no RDRAM or every cheat is OFF.
void cheats_tick();

}  // namespace ogre
