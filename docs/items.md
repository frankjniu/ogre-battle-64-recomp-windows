# Items, gold and the battery save

This file records where the game keeps items, how it grants them, and how the
battery save stores them. The evidence is `mips-linux-gnu-objdump -d` of
`build/*.elf` and the raw ROM. `mods/item-randomizer/` and the CHEATS tab
(`app/src/cheats.cpp`) are built on these addresses. All addresses are guest
VRAM unless marked ROM.

## Item definition tables

Two tables define the items. Both are `.streamedB` data (ROM `0x40E80` → RAM
`0x8016AF80`), so they are resident in every scene.

**Equipment**: `0x8018C42C` (ROM `0x6232C`), stride `0x20`, 279 records. Id 0 is
a dummy record, ids 1..277 are equipment (id 1 = Short Sword), and id 278 is
"None".

| offset | type | field | accessor |
|---|---|---|---|
| `+0x00` | u32 | name pointer into the pool at `0x8018B4D0` (ROM `0x613D0`) | `func_8016F5D0` |
| `+0x04` | u8 | category | `func_8016F5E8` |
| `+0x06` | u8 | rank | `func_8016F634` |
| `+0x08` | u16 | price (the shop reads it directly, bank AB `0x80215618`) | `func_8016F64C` |
| `+0x0A` | s8 | attack-like stat | `func_8016F67C` |
| `+0x10` | s8 | stat | `func_8016F664` |

The other fields (`+0x05`, `+0x0B..+0x0F`, `+0x11..+0x1C`) have accessors
`func_8016F600` and `func_8016F6CC`..`func_8016F9F4`; their meaning is not
traced.

**Consumables**: `0x8018E6EC` (ROM `0x645EC`), stride `0x0C`, 41 records. Id 0
is "None", ids 1..40 are consumables (id 1 = Heal Leaf).

| offset | type | field | accessor |
|---|---|---|---|
| `+0x00` | u32 | name pointer | `func_8016F500` |
| `+0x04` | u8 | class: 0 healing (ids 1..7), 1 stat items (9..20), 2 tools (8, 21..23), 3 medals (24..31), 4 quest items (32..40) | `func_8016F520` |
| `+0x05` | u8 | unknown | `func_8016F540` |
| `+0x06` | u16 | price (Heal Leaf 10) | |
| `+0x08`, `+0x09` | u8 | unknown | `func_8016F560`, `func_8016F580` |

Classes 3 and 4 are the key items ("Medal of Vigor" through "Pedra of Flame").

## The owned-item lists

The party owns items through two lists of 4-byte records:

| list | slots | ids | search (returns 511 when absent) |
|---|---|---|---|
| `0x80196B20` | 278 | equipment | `func_8016B6FC` |
| `0x80193AE0` | 40 | consumables | `func_8016B738` |

Record layout: `+0x00` u16 id (0 = empty slot), `+0x02` u8 used count,
`+0x03` u8 owned count (cap 99). `func_8017069C`, `func_801706B0` and
`func_80170688` read the id, `+0x02` and `+0x03` of an equipment record.
`func_80172820(id, count)` adds to or removes from the equipment list. New Game
clears the equipment list (`0x80185050`, 1112 bytes).

### The used count and the equip test

The item screens print `+0x02 / +0x03`. A unit can equip an item when
`+0x02 < +0x03` (`sltu`) on the record that `func_8016B6FC` finds first. The test
is in bank S (`func_ovlS_801D2F88`, `func_ovlS_801B3030`) and bank AF
(`func_ovlAF_80218910`).

`+0x02` counts roster references. `func_8016B774` (`.streamedB`, size `0x29C`)
rebuilds it: it clears the column of both lists, then adds one per reference:

| source | structure | ids | list |
|---|---|---|---|
| units | `0x80197210`, stride `0x19`, 30 records, used when `+0x01 & 1` | 10 bytes at `+0x0D` | 40-slot |
| characters | `0x80193BE0`, stride `0x38`, records 1..99, used when `+0x11 != 0` | 4 u16 at `+0x2A/+0x2C/+0x2E/+0x30` | 278-slot |
| class equipment | 4 accessors at `0x80186FF4` (`func_8016DFA8`, `func_8016DFFC`, `func_8016E050`, `func_8016E0A4`) over the class table `0x80187C62` (stride `0x48`) | 4 per character, from its class byte `+0x12` | 278-slot |

The save does not store `+0x02`; the save load calls the rebuild at
`0x8016CF30`. Other callers are `0x801728F8` (`func_80172820`), bank I
`0x801D6368`, bank J `0x801D8CAC` and bank M `0x8019F550`. Banks S and AB never
call it.

Four behaviours make `+0x02` go stale:

- The grant routine writes the id and `+0x03` only, so a reused empty record
  keeps its old `+0x02`.
- `func_ovlAF_80218E68` raises `+0x02` without comparing it with `+0x03`.
- Every search stops at the first matching record, so a duplicate id gets no
  references.
- `func_8016BA6C` (unequip) and `func_8016B774` do not test the 511 sentinel. An
  absent id makes them write `0x8019731E`, inside the unit records.

## The grant routine

`func_ovlN_801DC740` (bank N, record 7, ROM `0x101D00` → RAM `0x801AD5C0`)
grants one item. `a0` is the item word: bit 15 set selects the 278-slot list and
bit 15 clear the 40-slot list; the low 15 bits are the id. `a1` is unused. A
record that holds the id gets `+0x03 += 1` (clamped at 99); otherwise the first
record with id 0 gets the id and `+0x03 = 1` (`0x801DC7FC`, `0x801DC808`). It
never writes `+0x02`.

Callers: bank N `0x801ADEA8` and `0x801AE188` (map objects), `0x801BB934`
(ground pickups), `0x801E53DC` (battle reward); bank R `0x80215BEC`,
`0x80216D90` and `0x80217068`..`0x802171F0` (map-load grants).
`func_ovlN_801DD430` is a separate equipment-list add that also maintains
`+0x02`; the map loader equips class defaults through it.

## Where a granted id comes from

Each source below feeds one value to both the popup (the id field `0x801F36C2`)
and the grant. A source rewritten before the game reads it changes the message
and the inventory together. Keep each word's bit 15, because it selects the list.

**Table O, map objects.** `0x801EDB38` (ROM `0x142278`), 35 entries of 8 bytes,
zero-terminated: `{u16 object type, u16 item A, u16 item B, u16 item C}`. The
columns are three item categories. `func_ovlN_801AD6BC`, called from the scene
`0x03` enter at `0x8019F4AC`, reads it when `lbu(0x801976F8) & 0x40` is set: it
finds the entry for `lbu(0x80197708)` (`0x801ADD3C`), picks a column with
`rand() % 3` (`0x801ADD68`) and reads it at `0x801ADDB0`. A second gate is
`lbu(0x80193690) & 0x10`. Entry 0 is `0027 8031 809B 80E8` and entry 34
(`0x801EDC48`) is `0050 8091 80B2 80DF`.

**Per-unit pending ids.** With bit 6 of `0x801976F8` clear, the same function
reads `lhu(0x8019369C + unit*2)` (`0x801ADD14`) and clears it after the grant
(`0x801AE190`). `func_ovlN_801B15A0` fills these from record-7 data at
`0x801E85A4` (4-byte groups whose last byte is the item id), so they are
consumables.

**Table P, ground items.** `0x801F1002` u8 count, `0x801F1004 + i*2` u16 ids,
and bytes at `0x801F1024`, `0x801F1034` and `0x801F1044`.
`func_ovlR_80215C38` (bank R, record 9d) parses it at map load from asset
`0x021AF67C`, choosing the map's entry with
`lbu(0x801E7E36 + lbu(0x8018F4A1)*12)`. `func_ovlN_801B9694` reads
`idx = lw(0x801F0E24)`, stores the id in the popup field (`0x801BB8FC`), grants
it (`0x801BB930`) and sets bit `flag % 8` of `0x80196A58 + flag/8` from
`0x801F1044[idx]`. `func_ovlAE_8021B884` also reads table P.

**The battle-reward queue.** `0x801936D8`, 20 u16 in the saved game-state block.
`func_ovlN_801E49D0` reads `lhu(0x801936D8)` (`0x801E5230`, fallback
`0x801936DA` at `0x801E5254`), stores it at `0x8021C760`, grants it at
`0x801E53DC` and clears `0x801936D8` at `0x801E53F4`. The only constant-base
writer is the save-field loader `func_80185128`; the code that queues a reward
during a battle is not identified. The battle banks hold no drop table, and
`func_ovlJ_801D5DFC` computes the EXP award (message type 17), not an item.

**Map-load literal grants.** The tail of `func_ovlR_80215C38` grants literal ids
into the 40-slot list: `1,1,1,1,1,8,8,22,22` when `lbu(0x8018F4A1)` is 63 or 64
(`0x80217068`), otherwise `2×5, 3×5, 4×5, 5×5, 8×5, 21, 21, 22, 22, 22, 23, 23`
(`0x802170CC`).

**RAM sharing.** Table O, table P and the pending ids live in record 7's RAM,
which unit X (ROM `0x22A250`) and record 10b also use. Gate any write on record
7's image: `lhu(0x801EDB38) == 0x0027` and `lhu(0x801EDC48) == 0x0050`. The
reward queue is plain RAM.

## What a mod can hook

A code mod hooks only base-section functions (`docs/guides/app-build.md` →
"What a hook can reach").

- Base (`.streamedB`/`.main`): the two tables, `func_8016B6FC`, `func_8016B738`,
  `func_8016BA6C`, `func_80172820`, `func_80172C64` (class-change transfer), the
  accessors above, and the save-field reader `func_800749C0`. A hook on
  `func_8016B774` crashes the save load.
- Streamed banks, not hookable: `func_ovlN_801DC740`, `func_ovlN_801E49D0`,
  `func_ovlN_801B9694`, `func_ovlN_801AD6BC`, `func_ovlR_80215C38`, and the
  item screens in banks S, AB, AE and AF.

A mod reaches the streamed paths by writing RAM from the frame hook
`func_80072944`.

## Gold

Goth is a u32 at `0x80196A8C` (the army record `0x80196A78` + `0x14`). New Game
stores 1000 (`0x80184F10`). The battle reward adds to it and clamps at
`0x0098967F` = 9,999,999 (bank N `0x801E53B0`). The save keeps 31 bits.

## The battery save

OB64 saves to 32 KiB of SRAM. The Controller Pak is only a copy/backup device.

### Slots

| slot | device offset | size |
|---|---|---|
| 0, 1 | `0x10 + n*0x1850` | 6224 (`0x1850`) |
| 15 | `0x30B0` | 19176 (`0x4AE8`) |

| slot offset | contents |
|---|---|
| `0x000` | two u16 checksums over `slot+0x0C .. slot+0x1850` |
| `0x004` | the signature `QuestOG3` |
| `0x00C` | u32 in-use stamp; 0 = empty |
| `0x010` | 26-byte summary: `+0x00` u8 from `func_8016FF14`, `+0x01` ← `0x801936CB`, `+0x02` ← `0x801936CA`, `+0x04..+0x07` ← `0x80196A48`, `+0x08..+0x18` leader name in ASCII |
| `0x02A..0x184D` | the packed field blob |
| `0x184F` | one byte ↔ `0x8018F4A1` |

Both checksums are seeded with the slot's device offset: `checksum_sum` is the
byte sum plus the seed, and `checksum_bits` is the set-bit count plus the seed,
each `& 0xFFFF`. Data therefore validates only in its own slot.
`tools/sramsave.py` `reseed_slot` re-seeds a moved slot. Slot 15 is written at
the map; a zero first word passes validation without the checksum test
(`0x80075620`).

### Save code

| address | name | what it does |
|---|---|---|
| `0x80074CF0` | `save_image_read(dst, dev_off, size)` | reads the whole 32 KiB image once (256-byte DMAs) into `g_save_image`, then copies `size` bytes from `image + dev_off` to `dst` |
| `0x80074C58` | `save_image_write(image)` | writes `0x8000` bytes in 256-byte chunks, unless `*(0x800C4800)` is set |
| `0x80074BF0` | `save_commit(magic)` | writes the image when `g_save_dirty` is set and `magic == 0x37081383`, then frees it |
| `0x80074AD4` | `save_slot_summary_load(slot)` | validates the slot and returns its 26-byte summary, or NULL when the stamp is 0 |
| `0x8007541C` | `save_slot_validate(slot, buf)` | compares `QuestOG3` and both seeded checksums |
| `0x80075578` | `save_record15_validate(buf)` | the same for slot 15 (length `0x4ADC`, seed `0x30B0`) |
| `0x80075A84` / `0x80075B00` | `checksum_sum_seeded` / `checksum_bits_seeded` | the two checksums |
| `0x80075AC4` / `0x80075B60` | `checksum_sum` / `checksum_bits` | the same without a seed; the copy-to-pak path uses them |
| `0x800749C0` | `save_fields_load` | walks the field table and calls each entry's load function |
| `0x80093060` | `bcopy(src, dst, len)` | copies `a0` → `a1` (`0x800930BC`, `0x800930C4`) |
| `0x80092F50` | `memcmp` | |

| address | name | evidence |
|---|---|---|
| `0x800A83B8` | `g_save_image` (ptr) | `lui 0x800b; lw -31816` at `0x80075078`; stored at `0x8007525C` |
| `0x800A83BC` | `g_save_dirty` (u8) | `lbu -31812` at `0x80074BF4` |
| `0x800A8240` | `g_save_signature` (8 bytes, `QuestOG3`) | `addiu a0,a0,-32192` at `0x800754D4`, then `memcmp` |
| `0x800A824C` | `g_save_field_table[13]` (stride `0x1C`) | `func_800749C0` reads `+0x04` and `+0x0C` (`0x80074A70`, `0x80074A84`) |

### The field table

`0x800A824C`, 13 entries of `0x1C` bytes: `+0x00` function A, `+0x04` load,
`+0x08` save, `+0x0C` offset 1, `+0x10` load 2, `+0x14` save 2, `+0x18`
offset 2. A field's address is `slot buffer + 0x0C + offset`. Four entries are
used:

| idx | load | save | offset | block |
|---|---|---|---|---|
| 0 | `0x8016AF88` | `0x8016AF90` | `0x004` | the 26-byte summary |
| 1 | `0x8016CEC4` | `0x8016CF64` | `0x01E` | the packed field blob |
| 2 | `0x80173B60` | `0x80173B70` | `0x1843` | one byte ↔ `0x8018F4A1` |
| 3 | `0x80185118` / `0x80185128` | `0x80185120` / `0x80185984` | `0x1844` / `0xECC` | slot 15 only; `func_80185128` loads the 20 u16 at `0x801936D8` |

### The packed field blob

`func_800742F4` unpacks a bit stream into RAM and `func_80074494` packs RAM into
the stream. The blob's descriptor table is at `0x80187464` (slot 15 uses
`0x80187638`): 21 entries of `{u32 dest, u32 stride, u32 ops, u32 count}`. `ops`
points to 3-byte triples `{dest byte offset, byte count (+0x80 = signed), bit
width}`, ended by a triple whose second byte is 0. Bits are read MSB first; blob
bit 0 is the MSB of `slot+0x2A`.

| idx | dest | stride | count | bits/record | blob bit | block |
|---|---|---|---|---|---|---|
| 0 | `0x80193BE0` | `0x38` | 100 | 301 | 0 | characters (level `+0x13`, EXP `+0x35`) |
| 1 | `0x80197210` | `0x19` | 30 | 137 | 30100 | units |
| 2 | `0x801969D8` | `0x0B` | 10 | 64 | 34210 | |
| 3 | `0x800E9C08` | `0x12` | 1 | 61 | 34850 | boot-resident settings |
| 4 | `0x80196A48` | `0x10` | 1 | 96 | 34911 | |
| 5 | `0x80197830` | `0x06` | 121 | 29 | 35007 | |
| 6 | `0x80193698` | `0x3A` | 1 | 432 | 38516 | progress flags (Chaos Frame at `0x801936C9`) |
| 7 | `0x80197730` | `0x01` | 100 | 7 | 38948 | |
| 8 | `0x801977F8` | `0x01` | 30 | 5 | 39648 | |
| 9 | `0x80193AD0` | `0x01` | 6 | 3 | 39798 | |
| 10 | `0x80190FA0` | `0x16C` | 1 | 26 | 39816 | |
| 11 | `0x80190FA4` | `0x02` | 120 | 10 | 39842 | |
| 12 | `0x80191094` | `0x01` | 120 | 3 | 41042 | |
| 13 | `0x80195410` | `0x16C` | 1 | 24 | 41402 | |
| 14 | `0x80195414` | `0x02` | 120 | 10 | 41426 | |
| 15 | `0x80195504` | `0x01` | 120 | 3 | 42626 | |
| 16 | `0x80196A78` | `0x94` | 1 | 1095 | 42986 | army record (gold at `+0x14`) |
| 17 | `0x80193AE0` | `0x04` | 40 | 13 | 44081 | consumable list |
| 18 | `0x80196B20` | `0x04` | 278 | 16 | 44601 | equipment list |
| 19 | `0x80196A58` | `0x01` | 32 | 8 | 49049 | pickup flags |
| 20 | `0x80197188` | `0x01` | 16 | 8 | 49305 | |

Re-encoding entries 0..17 reproduces `slot+0x2A..slot+0x15F0` of a real save
byte for byte.

**Equipment list in the save.** Entry 18's ops (`0x80187448`) are `(00,02,09)`
and `(03,01,07)`: a 9-bit id and a 7-bit owned count per record. The field
starts at bit 1 of `slot+0x15F1` and ends at bit 0 of `slot+0x181D`, so records
are not byte-aligned. `+0x02` is not saved.

**Gold in the save.** Entry 16, offset `0x14`, 31 bits at blob bit 43130
(`slot+0x1539`, bit 2).

To change items through a save file, splice the bits and re-seed both checksums.
Writing `0x80196B20` live and letting the game save does both.
