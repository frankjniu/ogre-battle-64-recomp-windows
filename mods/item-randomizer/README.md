# Item Randomizer

**Experimental.** The behaviour can change between builds, and the mod has no
options. It rewrites items the game is about to grant, so back up any save you
use it on.

An Ogre Battle 64: Recomp code mod. An item the party receives after a battle or
finds on the map is replaced by a random item from the game's own item tables.

Shop purchases and key items (medals and quest items) keep the item the game
granted. Any other grant that fills a new inventory slot is rerolled, including a
non-key item handed over in a cutscene. The party's starting items are not
rerolled.

## How it works

The addresses below are explained in [`docs/items.md`](../../docs/items.md).

The game grants every item through `func_ovlN_801DC740`, which is streamed bank
code. A mod hooks only base-section functions, so the mod cannot hook the grant.
It runs from the base-section frame hook `func_80072944` (once per frame) and
works on RAM. Each frame it does three things.

**1. Rewrite the grant sources.** The game reads these values and passes one
word to both the popup and the grant, so a rewrite keeps the message and the
inventory in agreement:

- table O, the map-object drop table at `0x801EDB38`;
- table P, the per-map ground-item list at `0x801F1004`;
- the battle-reward queue, 20 u16 at `0x801936D8`. This is the only place a
  battle reward can be replaced.

Tables O and P share RAM with other banks, so the mod writes them only while
record 7 is resident (`lhu(0x801EDB38) == 0x27` and `lhu(0x801EDC48) == 0x50`).
Each word keeps its bit 15, which selects the list the grant fills.

**2. Watch both owned-item lists** (`0x80196B20` and `0x80193AE0`). A slot that
was empty and has just been filled is an acquisition, and the mod writes a
replacement id into it. In this case the message names the item the game rolled,
and the inventory shows the replacement. Two cases are not acquisitions:

- A slot that only changed its id is the game re-sorting the list.
- A frame that fills more than one record is a bulk grant: a save load, or a map
  load that grants starting items and class equipment. The mod re-reads both
  lists and rerolls nothing. A hook on the save-field reader `func_800749C0`
  flags a save load directly.

A merge into a stack the party already owns changes no id, so the list watch
cannot replace it.

Shop purchases are recognised by the gold they cost. Key items (consumable class
3 or 4) are kept as granted, because a chapter cannot be finished without them.

**3. Recompute the used count (`+0x02`)** of each record the game just filled.
The grant writes the id and `+0x03` only, so a reused record can inherit a stale
`+0x02` and read `1/1` under the new name. The game's rebuild `func_8016B774`
cannot be called from a mod (`jal` from `0x81000000` to game code does not
link) or hooked (the hook crashes the save load). The mod reimplements it from
the roster and writes the result only while the roster is in RAM.

The replacement map is stable for the life of the process: an original id always
becomes the same replacement. The mod marks every id it produced, so the three
mechanisms do not reroll each other's output. When a second grant would put an
id the party already owns into another record, the mod rolls a different item
(`dup-fix`). An acquisition with no valid replacement keeps the original item.

## Options

None. The start screen shows no rows for this mod, and it does not use
`mod_config/`.

| behaviour | value |
|---|---|
| replacement | anything in the matching item table |
| frequency | every eligible acquisition |
| seed | new every boot, from the frame counter |
| log | silent in a shipped build |

## Diagnostics

`LOG_LEVEL` in `src/item_randomizer.c` sets the log at compile time: 0 is silent,
1 reports each replacement and each save-load re-read, and 2 also reports every
acquisition. A frame prints at most 20 event lines and one summary. The log
needs the port's `recomp_log` export; a client without it refuses the mod with
`Imported function not found:*:recomp_log`.

| line | fields |
|---|---|
| `start verb`, `start seed` | log level and seed, once |
| `acquire` / `acquire-consumable` | `<id> <slot> <used +0x02> <count +0x03>` |
| `replace` / `replace-consumable` | `<from> <to> <slot> <list>` |
| `dup-fix` | `<original> <memoised> <fresh> <list>` |
| `keep-quest` | `<id> <slot> <class> <count>` |
| `count-up` | `<id> <slot> <old count> <new count>` |
| `drop-table` | `<from> <to> <entry*3+column>` |
| `pickup-table` | `<from> <to> <index>` |
| `reward-queue` | `<from> <to> <queue slot>` |
| `already-replaced` | `<id> <slot> <list> <count>` |
| `re-prime` | `<changed> <total> <list> <forced>`; `forced` is 1 after a save load |
| `acquire ref game` | `<id> <scan count> <game +0x02> <characters loaded>`; `DIFF` replaces `game` on a mismatch |
| `frame acq countup reroll` | `<frame> <acquisitions> <count-ups> <rerolls>` |
| `frame uses fixed stale skipped` | `<frame> <corrections> <stale records> <skipped>` |
| `use` | `<id> <slot> <old +0x02> <new +0x02>` |
| `probe first last present` | `<lhu 0x801EDB38> <lhu 0x801EDC48> <record 7 resident>` |
| `save-read` | the save-field reader ran |

`tools/refscan.py` rebuilds the `+0x02` column offline from an
`OGRE_DUMP_RDRAM` image and compares it with the game's bytes. Take the dump with
the party's items loaded.

```sh
make refscan DUMP=C:/tmp/run.bin                  # expect "mismatches 0"
python tools/refscan.py C:/tmp/run.bin --list     # every occupied record
python tools/refscan.py C:/tmp/run.bin --item 117 # which records credit one item
```

## Building

```sh
make mod-syms        # writes mods/reference/dump.toml (needs the linked ELF)
make example-mods    # writes build/mods/item-randomizer.nrm
```

`make example-mods` uses `mips-linux-gnu-gcc` from `PATH`, or
`gcc-mips-linux-gnu` in a `debian:bookworm-slim` Docker container. The build
passes `-mno-check-zero-division`, because `RecompModTool` cannot recompile the
`teq` that gcc emits after a division.

## Installing

Copy `item-randomizer.nrm` into the `mods/` folder next to the executable. The
start screen's MODS tab lists it, off by default. An installed mod makes the
start screen appear even when a ROM is ready; `OGRE_MOD_TEST=1` skips it for a
scripted run.

## Testing with a save

`run-with-save.sh` imports a battery save into its own config directory, installs
the built mod there, and starts the game:

```sh
make example-mods
mods/item-randomizer/run-with-save.sh build-app/saves/before-pickup.bin --reset
```

`--reset` re-imports the save. Without it, the run resumes from the progress the
previous run wrote back to the battery.

## Reading the code

* `src/item_randomizer.c`: the frame hook, the table rewrites, the list watch,
  the shop guard and the used-count rebuild.
* `include/modding.h`: the section macros (`RECOMP_HOOK`, `RECOMP_IMPORT`, …).
* `mod.toml`: the manifest.
* `mod.ld`: the link script.
