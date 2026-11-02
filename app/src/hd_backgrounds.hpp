// HD backgrounds: an external pack of PNGs that replaces the game's own
// dialogue/cutscene backdrops.
//
// The game builds every large backdrop on the njpeg path: four RSP-decoded
// sub-images are copied by `func_ovlE_8019976C` (bank unit E) into four fixed
// buffers, merged into a `'B5'` image, and blitted. That code is in a streamed
// bank unit, which a `.nrm` code mod cannot hook, so the port calls in from the
// generated unit instead: `tools/hd_backgrounds.py` places a call to
// `ogre_hd_background(rdram)` at the end of the readback copy.
//
// At that point the game's `state` object names the pass it just wrote:
//
//   state = *(u32*)0x8019A680
//   state[0x74]  destination buffer (the pass's pixels)
//   state[0x78]  destination width
//   state[0x7A]  destination height
//   state[0x80]  0 for the single-image path, which writes an 8-byte header at
//                `state[0x74]` and the pixels at +8; non-zero for `'B5'`, whose
//                pixels start at `state[0x74]`
//
// The current step and scene are read from the scripted-sequence words; the pack
// maps (scene, step) to a PNG. Each of the four passes gets its rectangle of the
// 496x384 canvas (`chunk_rect`; `docs/guides/hd-backgrounds.md`).
//
// The PNG is decoded once at startup, box-filtered to the canvas and converted to
// big-endian RGBA5551, then written over the pass's pixels in the runtime byte
// order (`MEM_H`/`MEM_B` in `recomp.h`). HD detail comes from the RT64 texture
// pack in `<pack>/hd`, which `tools/backgrounds.py hd` cuts from the same PNG.

#pragma once

#include <cstdint>
#include <filesystem>

namespace ogre {

// Loads `mods/backgrounds/` from the first candidate directory that holds a
// `backgrounds.txt`: `$OGRE_BG_DIR`, `<pref_dir>/mods/backgrounds`,
// `<pref_dir>/../mods/backgrounds`, or `<cwd>/mods/backgrounds`. Called once at
// startup from `main.cpp`; a missing pack is not an error.
void hd_backgrounds_init(const std::filesystem::path& pref_dir);

// Whether the pack has an entry for a scene. The widescreen toggle uses this to
// expand the scenes whose backdrop comes from the pack, so the replacement fills
// the wide window instead of being pillarboxed (`widescreen.cpp`).
bool hd_backgrounds_covers_scene(uint16_t scene);

// The RT64 texture pack generated for the pack's backgrounds
// (`tools/backgrounds.py hd`): `<pack>/hd` when the pack is loaded and that
// folder holds an `rt64.json`, else empty. `OGRE_BG_HD=0` returns empty, which
// keeps the game-resolution write and drops the HD strips. The renderer loads
// it with `TextureCache::loadReplacementDirectory`.
std::filesystem::path hd_backgrounds_texture_pack();

// A screen backdrop: the pack image for a screen whose own draw the port
// patches to show it (the title, scene 0x04, the Hugo Report, scene 0x08, and
// the Load Game book, scene 0x12; screen_backdrops.cpp). The image is
// box-filtered to 240 rows with its aspect kept, its width rounded to a
// multiple of 4 texels, and centred on the 320-wide screen, so an image wider
// than 4:3 fills the widescreen sides. The draw loads it in strips of
// `strip_rows` rows (one 4 KB TMEM load each) from a staging buffer in RDRAM;
// every strip's pixels are unique, so RT64 can key an HD replacement to each.
struct ScreenBackdrop {
    const char* name = "";
    const char* id = "";
    int width = 0;       // texels, a multiple of 4
    int height = 0;      // 240
    int x = 0;           // screen x of column 0 (160 - width / 2)
    int strip_rows = 0;  // rows per TMEM load
    const uint16_t* pixels = nullptr;  // RGBA5551 values, row-major, opaque
};

// The scenes whose backdrop is a screen backdrop rather than an njpeg canvas.
bool hd_backgrounds_is_screen_scene(uint16_t scene);

// The pack's backdrop for a screen scene, or null. `wide` picks the `-wide`
// image when the pack has one (the WIDESCREEN setting is on).
const ScreenBackdrop* hd_backgrounds_screen(uint16_t scene, bool wide);

// Rows per strip for a backdrop `width` texels wide: as many 16-bit rows as fit
// in TMEM's 4 KB.
int hd_backgrounds_screen_strip_rows(int width);

}  // namespace ogre
