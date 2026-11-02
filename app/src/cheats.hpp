// Cheats from the CHEATS tab of the launcher and the Esc overlay.
//
// Each cheat is a toggle in settings.hpp (`Cheat`, plus the TIME OF DAY row
// `cheat_hour`). The cheats are the GameShark list for this ROM (USA v1.1; the
// header's version byte `0x3F` is 1). Two kinds exist:
//
// * Data cheats are held every frame like a GameShark constant write: the game
//   may change the value, and the next frame restores it. `cheats_tick()` does
//   this from the main thread.
// * Code cheats patch an instruction. Here the code is compiled C, so writing
//   the instruction word into RDRAM changes nothing. Each one is a
//   `[[patches.hook]]` in config/banks/config-bankJ.toml that calls a function
//   below, and the function reproduces the patched instruction's result while
//   the cheat is ON.
//
// Turning a cheat OFF stops it and leaves the current values. Saving while a
// cheat is ON stores the cheated values in the battery save. The published
// GameShark codes for USA v1.0 sit 0x20 bytes lower in the army block
// (docs/symbols.md -> goth).
//
// * MAX GOTH (GameShark "Infinite Goth", `81196A8C 0098` / `81196A8E 967F`):
//   goth, a u32 at `0x80196A8C`, held at 9,999,999, the game's own cap. The shop
//   compares it with the price (`lw` at `0x8021564C`, bank AB) and the battle
//   reward adds to it and clamps at `0x0098967F` (bank N, `0x801E53B0`).
// * MAX ITEM COUNTS: every non-empty slot of the two owned-item lists held at
//   99, the game's cap. The lists are the 278-slot `0x80196B20` and the 40-slot
//   `0x80193AE0`, 4 bytes a slot: `+0x00` u16 item id (0 = empty), `+0x03` u8
//   count. The game's own debug cheat (`func_ovlS_801C214C`, `0x801C25B0`) walks
//   the same two lists with the same empty test and sets 98.
// * QUICK LEVEL GAIN (`D11D639C 2C42` / `811D639E 0000`): the D1 line tests
//   that bank J (record 10a, the battle-result module, ROM `0x213AE0`) is
//   resident; the write turns `sltiu v0,v0,100` at `0x801D639C` into
//   `sltiu v0,v0,0`. That is state 2 of `func_ovlJ_801D61C0`: `v0` = the
//   unit's experience (`lbu v0,50(s0)` at `0x801D6398`), and `bnezl` at
//   `0x801D63A0` skips the level-up (`func_ovlJ_801D8E90`, then
//   `sb zero,50(s0)`) while it is below 100. The hook before `0x801D63A0` sets
//   `v0` to 0, so every unit that reaches the check levels up.
// * QUICK SOLDIER PROMOTION (`D11D639C 2C42` / `811D25AA 9100`): the same D1
//   test; the write turns `sll s2,s2,1` at `0x801D25A8` (`func_ovlJ_801D204C`)
//   into `sll s2,s2,4`. The port does not copy the code: at the developer's
//   request (2026-11-01) one battle is always enough, so the hook before
//   `0x801D25DC` raises the counter to 100..255 whenever the battle earned
//   points. The game's mechanism: `s2` is the sum of `func_801C9FD4(+0x20, +0x22)`
//   (HP in thirds of max HP, 0..3) over the participants with `+0x4C == 1` and
//   flag bits 0, 1 and 8 clear (`0x801D2540`-`0x801D25A0`); after the shift (and
//   a second `sll s2,s2,1` when `func_801CC53C(24)` is non-zero) it is added to
//   byte `+0x37` of the leader's roster entry, `lbu(s3+0xF6)` with `s3` from
//   `func_801C93A0` (`0x801D25BC`-`0x801D25E4`). `func_ovlJ_801D12B0` tests that
//   byte: at 100 or more it caps it at 99 and, when the roster has fewer than 94
//   named units and the inventory has a spare of each starting item of the new
//   class, `func_ovlJ_801D0B80` replaces one `+0x4C == 1` participant (a generic
//   soldier, squad member id 100+) with a new roster unit of class 2 (Fighter)
//   or 4 (Amazon) from the leader's byte `+0x14`, resets `+0x37` to 0 and adds
//   20 to the leader's `+0x36`. The byte wraps at 256. A squad with two
//   soldiers at full HP earns 12 points a battle, and 96 with the GameShark
//   code, so the code needed two battles. The check runs in state 8, after the
//   points (state 9) and the experience (state 5, `func_ovlJ_801D61C0`, which
//   sets state 8 at `0x801D6460`), so a filled counter promotes at the same
//   battle result.
// * UNLOCK ALL CLASSES (`81196A96 F4FF` ... `80196AA0 0001`): the class list's
//   bitfield at `0x80196A96`, one bit a class id: byte `class / 8`, mask
//   `1 << (class % 8)` (table at `0x801EF3D0`, bank S). The organize module
//   sets the bit of every roster unit's class (`func_ovlS_801D736C`,
//   `0x801D73E4`-`0x801D740C`) and the class list reads it
//   (`func_ovlS_801CF36C`, `0x801CF418`). The cheat ORs the GameShark bytes in
//   every frame: the code's bytes leave class ids 0, 1, 3, 42 and 44 clear, and
//   an OR keeps any of those the game set itself.
// * DAY ALWAYS 00 (`811F108C 0000`): the mission field's day counter, a u8 at
//   `0x801F108C` in the BSS of record 7 (unit N, ROM `0x101D00`). The mission
//   clock `func_ovlN_801B134C` adds 1 each time the game date changes
//   (`0x801B141C`-`0x801B1434`, capped at 99), `func_ovlN_801AD6BC` clears it
//   (`0x801ADA34`), and the field HUD draws it as two digits (`0x801BD168`).
//   Held at 0 (the code's halfword write) only while record 7 is resident: the
//   same RAM holds unit X's battle bank (record 10c) or unit S's record 06 at
//   other times.
// * TIME OF DAY (`80196A4D 00XX`): the hour, a u16 at `0x80196A4C`; the code
//   writes its low byte. The main clock `func_8016C81C` advances it at minute
//   60 and wraps it at 24 (`0x8016C8A4`-`0x8016C8D0`), then advances the date
//   (`func_8016C5BC`); New Game sets 6 (`0x8016C960`). The mission clock reads
//   the byte (`lbu` at `0x801B147C`). Held at the chosen hour: the clock still
//   runs, so each in-game hour still fires the hourly work, and hour 23 rolls
//   to 24 and advances the date before the next frame restores 23.
#pragma once

#include <cstdint>

namespace ogre {

// Applies the data cheats that are ON to the running game's RDRAM. Called once
// per frame on the main thread, from the frame callback in main.cpp. A no-op
// while the runtime has no RDRAM or every data cheat is OFF.
void cheats_tick();

}  // namespace ogre

// The bank J hooks (config/banks/config-bankJ.toml). They run on the game's
// threads, inside the recompiled bodies.
extern "C" {
// QUICK LEVEL GAIN: the result of `sltiu v0,v0,100` at `0x801D639C`; returns
// 0 while the cheat is ON, else `below_100` unchanged.
uint64_t ogre_cheat_level_check(uint64_t below_100);
// QUICK SOLDIER PROMOTION: `counter` is `v0` after `addu v0,v0,s2` at
// `0x801D25D8` (the leader's byte `+0x37` plus the battle's points) and
// `points` is `s2`. While the cheat is ON and `points` is above 0, returns
// `counter` clamped to 100..255; otherwise returns it unchanged.
uint64_t ogre_cheat_promotion_counter(uint64_t counter, uint64_t points);
}
