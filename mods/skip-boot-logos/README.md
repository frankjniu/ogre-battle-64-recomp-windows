# Skip Boot Logos

An example mod for Ogre Battle 64: Recomp: one code mod, one hook, one toggle in
the client. It is the smallest reference for the mod API.

## What it does

The game runs three scenes before anything else:

| scene | what it shows | length |
|---|---|---|
| `0x09` | boot intro: soldiers, a falling cube, the Nintendo 64 logo | ~9.4 s |
| `0x0A` | publisher stills: Licensed by Nintendo / ATLUS / QUEST | ~16.3 s |
| `0x04` | title: prologue text over the scrolling clouds | — |

The cartridge save is read before the first scene, so the title is reached with
the battery already loaded.

Each scene's per-frame update writes the next scene into the pending-scene word
`D_800C4C26` as `0x8000 | id`, and the dispatcher switches on the next frame.
The mod puts an entry hook on scene `0x09`'s update and writes the title's id:

| hook | function | writes |
|---|---|---|
| scene `0x09` update | `func_80177DCC` | `0x8004` (the title) |

The boot then runs `0x09` for one frame and enters the title.

Do not write the word from scene `0x0A`'s update instead. Scene `0x0A`'s enter
and leave set up and tear down globals in overlay C that only its completed state
machine leaves consistent. Cut short, it leaves the title with no display lists,
and the last presented frame stays on screen. Skipping `0x0A` entirely works.

## Building

The mod compiles to big-endian MIPS, so it needs a MIPS cross compiler. From the
repository root:

```sh
make mod-syms        # writes mods/reference/dump.toml (needs the linked ELF)
make example-mods    # writes build/mods/skip-boot-logos.nrm
```

`make example-mods` uses a `mips-linux-gnu-gcc` on `PATH` when there is one and
falls back to running `gcc-mips-linux-gnu` in a `debian:bookworm-slim` Docker
container.

## Installing

Copy `skip-boot-logos.nrm` into the `mods/` folder next to the executable. The
start screen's MODS tab lists it, off by default.

## Reading the code

* `src/skip_boot_logos.c` — the hook and why it targets scene `0x09`.
* `include/modding.h` — the section macros (`RECOMP_HOOK`, `RECOMP_PATCH`,
  `RECOMP_EXPORT`, `RECOMP_IMPORT`).
* `mod.toml` — the manifest.
* `mod.ld` — the link script.

A mod can also declare options and read them with `recomp_get_config_u32` and
friends; the client's start screen edits them. This mod needs none.

`docs/scenes.md` has the boot scene descriptors, and `tools/scenemap.py scenes`
reprints them from the ROM.
