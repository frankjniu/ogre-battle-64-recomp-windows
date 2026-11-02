# What each screen shows

The developer's description of each screen (AGENTS.md rule 1), with the scene
id, descriptor and the bank unit that draws it. `dev` marks a description from
the developer; `code` marks one read from the ROM, which is unverified intent.
Every screen below renders in the port.

Retail reference screenshots from the developer:
`docs/proofs/intro-movie-reference/retail-shot-1..4.png` (the closing intro
movie) and `docs/proofs/map-reference/retail-map-screen.png` (the map after the
prologue).

## How the game is organised

- **25 scene types** (`0x00`..`0x18`). `func_80075BC0` fills `D_800AF028[0..24]`
  with accessors; each returns a descriptor of five words: `+0x00` enter,
  `+0x04` update, `+0x08` hook, `+0x0C` leave, `+0x10` bank-record mask (bit N =
  record N). Some accessors pick one of two descriptors on a mode word.
- **Transitions:** 39 store sites in 30 functions write the pending scene
  `D_800C4C26` (`0x8000|id`). That is the whole scene graph.
- **Content:** a 1693-entry scripted step table (asset `0x19A8804`) indexed by
  `D_8018F1C0 & 0xFFF`. The 200+ dialogues are steps played by scene `0x0D`,
  loaded through the one-frame scene `0x02`.
- `tools/scenemap.py` prints all of this from the ROM. `tools/arenamap.py` lists
  every streamed bank and the unit that compiles it.

## Scene index

| id | screen | descriptor | unit |
|---|---|---|---|
| `0x02` | one-frame loader in front of `0x0D` | `D_8018FC50`, enter `func_80178920` | |
| `0x03` | mission field; tutorial practice stage when `D_80193700 != 0` | `0x8018F350` (mask `0x38C`) / `0x8018F364` (mask `0x4038C`) | N, P, Q, R and the other record-9 banks |
| `0x04` | title | | |
| `0x05` | world map | `0x8018FD70`, mask `0x2` | M |
| `0x06` | Organize Screen | `0x8018FD84` (mask `0x2`) / `0x8018FD98` (mask `0x40002`) | S |
| `0x07` | name and birthday forms | `D_8018FDAC`, enter `func_8017B794` | H |
| `0x08` | Hugo Report | `0x8018FC64` | |
| `0x09` | boot intro | | |
| `0x0A` | publisher stills | | |
| `0x0B` | attract story (world map) | | |
| `0x0C` | attract unit-info book | | |
| `0x0D` | movie and dialogue engine | `D_8018FC3C` | C, F, G, J, K, L |
| `0x0E` | battle | `0x8018FB2C`, mask `0x3C00` | X, W, AH |
| `0x11` | credits | `0x8018FBAC`, mask `0x8000` | main (`streamedC`) |
| `0x12` | Load Game | | |
| `0x13` | Chaos Frame total after the credits | `0x8018FBD4`, mask `0x8000` | |
| `0x14` | Witch's Den | `0x8018FBE8`, mask `0x13C14` | AG |
| `0x16` | closing intro movie | `0x8018FC00`, mask `0x400` | I |
| `0x17` | tutorial | `0x8018FE50` (mask `0x60000`) / `0x8018FE64` (mask `0x20000`) | B, T |
| `0x18` | Controller Pak menu | `0x8018FDC0`, mask `0x2` | H |

## Boot and attract

| screen | scene | source |
|---|---|---|
| soldiers, a falling cube, the Nintendo 64 logo | `0x09` | code |
| publisher stills: Licensed by Nintendo, ATLUS, QUEST (640x480). The QUEST still is three overlapping 3D "q" shapes (blue/green, yellow, red) over the `QUEST` wordmark | `0x0A` | dev |
| title: prologue text, then the logo and a menu over scrolling clouds | `0x04` | dev |
| attract story over the world map; attract unit-info book | `0x0B`, `0x0C` | code |
| Controller Pak menu: `Save / Load / Erase / Exit` over `OgreBattle Game Notes`, the note list and the system messages. Reached by holding Start through the boot (`0x800721DC` tests the pad report `D_800E79B0 & 0x1000`) | `0x18` | dev |

The title menu is `New Game / Tutorial / Stereo`; with a battery save it is
`New Game / Load Game / Tutorial / Stereo` and the cursor starts on
`Load Game`. `Stereo` toggles with left/right. `Load Game` (`0x12`) lists the two
battery slots and continues to the map (`0x05`).

The game saves to the cartridge battery (SRAM). The Controller Pak menu is the
copy/backup path between the cartridge and a pak; its copy and erase flow is out
of scope (dev).

## New Game opening

The opening is repeated `0x02 → 0x0D` visits, one step per visit, with the step
in `D_8018F1C0`. Drive it from the title (AGENTS.md durable facts); seeding
`OGRE_STEP` changes the sequence's own state.

| # | what the player sees | scene, step | source |
|---|---|---|---|
| 1 | a multi-shot sepia cutscene in a castle courtyard, ending on *"I promise I'll make you proud."* (~28.7 s); a button press skips it | `0x0D`, step 1 (movie mode) | dev |
| 2 | a card (`Ischka Military Academy / Graduation Ceremony`), then the player walks to Archbishop Odiron in a cathedral: a pre-rendered 320x240 backdrop (red carpet, columns, organ, animated candles) and dialogue lines | `0x0D`, step 2 | dev |
| 3 | Odiron asks the name: a name box (`Magnus`), an `A–Z` / `a–z` grid, `INS BS DEL END`, a `Yes/No` prompt | `0x07` | dev |
| 4 | date of birth: a `BIRTHDAY` banner, `Jul. 25`, a second row `Trueno 12` | `0x07` | dev |
| 5 | personality questions over a live hall scene (`"What dost thou hold within thy sword?"`, six answers); the answers decide the starting units and items | `0x0D` | dev |
| 6 | the closing intro movie, one continuous scene in five phases: `ATLUS USA presents`; `Developed & licensed by Quest / Nintendo`; `Ogre Battle Saga Episode VI`; `Person of Lordly Caliber` over the flower of Lodis; a montage of the party travelling (campfire, a cliff at sunset, the parchment world map with a red route) | `0x16`, steps 970..974 | dev |
| 7 | a chapter animation: characters revealed over `Prologue` / `Casting their gaze on the ground, trudging along...` | `0x0D`, steps 1073..1077 (command `-8`, unit K) | dev |
| 8 | the player received for duty: General Godeslas over rows of soldiers, then the Grey-Haired Old Man and Magnus | `0x0D` | dev |
| 9 | the world map | `0x05` | dev |

A step's command comes from the last word of its decompressed descriptor: when
the high byte is `0xFF`, the low byte (1..8) indexes the table at `0x8022ABE0`,
which maps it to one of `-3 -6 -5 -4 -7 -8 -9 -10`; `func_ovlC_8022683C` indexes
`0x8022ABA0` with `command + 10` to pick the setup arm and its bank DMA.

A town-street cutscene in the same engine (from the developer's `pre_ogre.bin`
save): a stone street, a knight in the middle with a balloon *"Help! Someone,
help me!!"*, and a fallen soldier below him. The knight's head is a separate part
of the flat unit sprite (`docs/guides/sprite-mechanisms.md`).

## World map (`0x05`)

The framed world map (dev): terrain and rivers, location labels, the road of
dots, the player's party (a blue knight, Magnus) and other unit markers, a cursor
(white arrow and red dot) on the selected location, and a `MONTH` / `DATE` panel
in the bottom right with the date in an italic serif face (`Sombra 1`).

- `A` works only with the cursor on a location (its label shows). `START` opens a
  tooltip that blocks input until dismissed.
- The knight is composited at run time into a `malloc(0x18000)` RGBA32 sheet at
  `state[+0x34]` (24 frames of `0x1000`); `state = *(u32*)0x80197B18`. The map
  sprites are faithful (AGENTS.md durable facts).
- The map cursor is `state+0x52/+0x54`, written by `app/src/map_cursor.cpp` at the
  cursor draw `0x801A1C48` when MOUSE is on.

**R menu** (dev): `Organize` (default), `Hugo Report`, `Settings`, `Save`.

- Hugo Report (`0x08`): Hugo the Tactician's portrait and text, a `MENU` window
  (`Events`, `Miscellany`, `Tips`) and a `TIME` window over a stone tablet with a
  griffin relief.
- Settings: game and text speed and other options (`Message speed`,
  `Cursor speed`, `Help display`, … `Restore defaults`); unit V.
- Save: two slots stacked vertically; choosing one asks *"Any existing data will
  be overwritten. Proceed?"* with `Yes` as the default.

## Mission (`0x03`)

3D terrain with cliffs, woods and rivers, and 2D characters over it (dev): the
party sprite with selection brackets, a `Stronghold` tooltip with the `R` hint,
the unit panel (`No. / FRIENDLY / STATUS`), the location label with its
illustration, and the `MISSION` banners along the bottom.

- **Intro:** the camera pans from the party's fort to the enemy fort, the `NOTE`
  winning condition appears, the camera returns, the losing condition appears,
  then `MISSION START`. Each phase waits for `A` (dev). The target fort is a
  framed illustration with its place label and a red pin.
- **Field cursor:** the D-pad moves it and the camera follows (dev). It is two
  f32 pixels at `state+0x08/+0x0C` (`state = 0x800E91D0`), moved by
  `func_ovlN_801BE4E0`; `app/src/mission_cursor.cpp` drives it from the mouse.
- **R menu:** a hold menu, drawn only while `R` is down. Entries include
  `Dispatch` (first), `Mission Objective` and `End`; the full order is not
  transcribed.
- **Combat** on the field (units W and X).
- **Neutral encounter:** a rare event where a moving unit meets a wild monster
  (`Magnus: "A wild Young Dragon? Here!?"`); the battle can persuade it to join
  (dev). The wild unit is created by the setup fragment ROM `0x23A370` → RAM
  `0x801D0860` (unit AH).
- **Shop** in a town (unit AB).
- **Witch's Den** (`0x14`): a shop interior with a cauldron and shelves, a
  `WAR FUNDS` counter and the Old Witch (*"Heh heh heh... Can I help you?"*).
  One city per mission has it, and visiting it revives dead soldiers (dev).

Loading a suspend save resumes straight into `0x03`; the natural route from the
map plays `0x02 → 0x0D` (a story step) `→ 0x16 → 0x02 → 0x0D → 0x03` and streams
a different bank of record 9's arena (unit R, the enemy/unit constructor). A
screen that works from a save can still be missing code when played to.

## Organize Screen (`0x06`)

The army-management screen, also shown before a battle (dev): counters across the
top (`SOLDIER 030`, `CHARACTER 25`, `UNIT 03`), the formation grid of characters
on a chequered floor of stepped tiles, the selected entry's name plate at the
bottom right, and the `WAR FUNDS` plate at the bottom left. The enter DMAs ROM
`0x87220` (`0x56D60`) → `0x8019A7C0` and calls `0x801C19B0`.

## Tutorial (`0x17`)

Deneb's dialogue box over a dark blue textured backdrop (`Welcome! / Is this
your first time here?`), then the lesson menu. A lesson runs `0x17 → 0x02 →
0x0D →` the practice stage: scene `0x03` with `D_80193700 == 1`, the tutorial
instruction box over the field (unit T, ROM `0x1C32D0`). Record 18's loader
`func_ovlB_80221D50` streams the per-mode module from a table at `0x80229DDC`;
mode 2 is unit U, which no observed route loads.

## Ending

| screen | scene | source |
|---|---|---|
| fade from black to `Ogre Battle 64` in the middle of the screen | `0x11` | dev |
| scrolling credits over backgrounds from the game | `0x11` | dev |
| the total Chaos Frame the player finished with | `0x13` | dev |
| back to the attract loop | `0x09` | code |

Test the credits from a save before the final boss; a forced `OGRE_SCENE=0x11`
plays the opening instead.

## Not yet described by the developer

- The full order of the mission's `R` menu.
- The Organize Screen's controls.
- The Witch's Den revive flow (a list of dead soldiers, a price, a
  confirmation).
