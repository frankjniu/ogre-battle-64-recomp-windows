# Documentation

The Windows recomp of Ogre Battle 64 (USA, Rev A). The to-do list is
[`../PLAN.md`](../PLAN.md). Per-session history is the git log; the per-session
handoffs and the status log were removed on 2026-10-30 and are in git history.

| Document | Purpose |
|---|---|
| [../AGENTS.md](../AGENTS.md) | Working rules for AI agents, and the durable technical facts. Read first. |
| [../PLAN.md](../PLAN.md) | Where the port stands, and the prioritized to-do list. |
| [DECISIONS.md](DECISIONS.md) | The decisions that still bind the port, by area. |
| [guides/app-build.md](guides/app-build.md) | Windows setup (Build Tools, code generation in WSL, traps), building, running, packaging, releases, every `OGRE_*` knob, the SETTINGS rows, mods, and the diagnostics toolkit. |
| [guides/emulator-first.md](guides/emulator-first.md) | Read before interpreting a display list: what counts as evidence, the five faithfulness checks, hashing a ucode, borrowing a reference emulator. |
| [guides/app-architecture.md](guides/app-architecture.md) | The app's layout, runtime flow and callbacks. |
| [guides/rsp-microcode.md](guides/rsp-microcode.md) | Which RSP microcodes are recompiled (njpeg, audio) and how. |
| [guides/njpeg-backgrounds.md](guides/njpeg-backgrounds.md) | The njpeg background pipeline: assets, tile geometry, the readback, how to check one. |
| [guides/hd-backgrounds.md](guides/hd-backgrounds.md) | The `mods/backgrounds/` PNG pack that replaces dialogue/cutscene backdrops. |
| [guides/sprite-mechanisms.md](guides/sprite-mechanisms.md) | How sprites and text are drawn: the part cache, the cutscene draw list, the map's sprite rectangles, checking a draw. |
| [scenes.md](scenes.md) | What each screen should show, from the developer (the AGENTS rule 1 oracle). |
| [symbols.md](symbols.md) | Proposed symbol names: address → name → evidence → confidence. |
| [notes-bank-hooks.md](notes-bank-hooks.md) | Design for mod hooks on streamed-bank functions (to do). |
| [notes-item-re.md](notes-item-re.md) | Item grant paths: battle drops and map pickups. |
| [notes-drop-table.md](notes-drop-table.md) | Drop and pickup item-id tables, and how the game reads them. |
| [notes-tutorial-drops.md](notes-tutorial-drops.md) | Where the tutorial's granted item ids come from. |
| [notes-item-equipped.md](notes-item-equipped.md) | The owned-item list: the `x/y` pair, the `+0x02` used count, the equip test. |
| [notes-save-items.md](notes-save-items.md) | Owned items in the battery save and their live RDRAM copy. |

The item notes are the reference for `mods/item-randomizer/`. The mod READMEs
are in `../mods/*/README.md`.
