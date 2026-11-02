# RSP microcode

The runtime sends every gfx task (`M_GFXTASK`, type 1) to the renderer's
`send_dl`, and RT64 interprets the display list with its own GBI interpreters
(`tools/RT64/src/gbi/`). RSPRecomp compiles two of the game's other microcodes:
the njpeg decoder (`M_NJPEGTASK`, type 4) and the audio driver (`M_AUDTASK`,
type 2). `app/src/rsp.cpp` dispatches them by ucode address through
`recomp::rsp::get_rsp_microcode`. Any other task gets a stub that completes it
without running microcode; `tools/runlog.py` flags such a task (AGENTS.md
rule 8).

`make rsp-recomp` (WSL) writes `RspFuncs/njpeg_ucode.cpp` and
`RspFuncs/audio_ucode.cpp` (gitignored C++, built against librecomp's `RSP`
vector unit into `ogrebattle64_rsp`).

## The microcodes

Main-segment ROM offset = `vram - 0x80070C60 + 0x1060`.

| task | ucode text | ucode data | GBI / role |
|---|---|---|---|
| gfx (type 1) | `0x8009F540` (ROM `0x2F940`, `0x1000`) | `0x800AC140` (ROM `0x3C540`, `0x800`) | `F3DEX2.fifo 2.08`; RT64 auto-detects it |
| njpeg background draw (type 1) | `0x800A5110` | | `S2DEX2.fifo 2.08`; RT64 auto-detects it |
| njpeg decode (type 4) | `0x8009ED80` (ROM `0x2F180`, `0x7C0`) | `0x800AC050` (ROM `0x3C450`, `0xF0`) | recompiled, `config/rsp-njpeg.toml` |
| audio (type 2) | `0x8009E050` (ROM `0x2E450`) | `0x800ABDA0` (ROM `0x3C1A0`, `0x800`) | recompiled, `config/rsp-audio.toml` |

Types 2 and 4 share the libultra boot loader at `0x8009ECB0` (ROM `0x2F0B0`,
`0xD0`). It DMAs `ucode_data` to DMEM 0, DMAs the text to IMEM `0x1080`, and
jumps there.

`docs/guides/emulator-first.md` §3 is the recipe to confirm which GBI RT64
picked for a list.

## Config rules

Both rules apply to any microcode recompiled here.

- **`text_address` is a label base: the IMEM address the text was loaded at,
  masked `0x1FFF`.** RSPRecomp labels instruction *i* as
  `(text_address + 4i) & 0x1FFF` and resolves `j`/branch targets the same way.
  Both microcodes load at IMEM `0x080`, so both use `0x1080`. Any other value
  rotates every jump target: the njpeg decoder then writes one block per task,
  and the audio driver never DMAs its command list.
- **`text_size` ends at the microcode's last real instruction.** The njpeg text
  is `0x7B8` (the last 8 bytes of the declared `0x7C0` are data). The audio text
  is `0xC60` (the loader DMAs a fixed `0xF80`, which runs into other code).
  Compiling past the end emits `goto`s to labels that do not exist.

## njpeg decoder

The game calls the CPU Huffman decode `func_8008B250(payload, buffer)` first,
then submits one type-4 task per image. `data_size` carries the macroblock
count and `yield_data_size` the quantization scale. Each macroblock comes out
as 768 bytes of 16-bit YUV (4x128 Y, 128 U, 128 V, 256 unused). The format is
the N64 Programming Manual's JPEG chapter. The rest of the pipeline (the S2DEX2
draw, the colour conversion, the CPU readback) is in
`docs/guides/njpeg-backgrounds.md`.

## Audio driver

The driver dispatches commands through a halfword table in `ucode_data` at
DMEM 0 (`lh at,0(at); jr at` at IMEM `0x10DC`). RSPRecomp cannot see those
targets, so `extra_indirect_branch_targets` in `config/rsp-audio.toml` lists
every handler the 16-entry table can produce, plus `0x10B4` (the command-list
DMA helper returns there through `jr $5`).

`SETLOOP` (opcode `0x0F`, handler `0x1384`) stores its loop address at DMEM
`0x0E`, over table entries 7 and 8. The game never emits opcodes 7 or 8, and the
store is faithful. `audio_ucode_guard` in `app/src/rsp.cpp` logs a non-`Broke`
return and reports the task complete instead of letting the runtime exit.

Each audio task's reply comes only from the game's scheduler, after the RSP
finishes (`docs/DECISIONS.md` → Runtime).

### Checking the audio output without listening

The game's AI descriptor is at `*(0x800A9B90)`: `+0x00` buffer, `+0x04` length
in words. Dump RDRAM while music plays and read that buffer; it holds non-zero
PCM when the driver runs.

```sh
printf 'c\ndump C:/tmp/live.bin\n' > C:/tmp/oc.tmp && mv C:/tmp/oc.tmp C:/tmp/ogre-console.txt
OGRE_NO_AUDIO=1 OGRE_CONSOLE_AT_MS=5000 OGRE_CONSOLE_FILE=C:/tmp/ogre-console.txt \
  OGRE_EXIT_AFTER_MS=7000 "$APP" "$ROM"
tools/rdram.py C:/tmp/live.bin word 0x800A9B90
```

## Escape hatches

`OGRE_NJPEG=0` and `OGRE_AUDIO_UCODE=0` force the stub for one microcode, for an
A/B run. The stubbed audio task is silent; the buffers still queue and drain.
