// The cutscene engine's vignette (scene 0x0D) in widescreen.
//
// Some cutscenes, the sepia intro movie among them, darken the picture's
// edges with a ring of black vertices: four border quads and one centre quad,
// drawn by `func_ovlL_8022BE50` (unit L, bankRec14a at 0x8022ACB0). The
// function copies the ring's ten vertices from the cutscene's data
// (`**(engine + 0x1A48)`, engine = `*(u32*)0x8022A994`) into the frame's vertex
// buffer at `*(u32*)0x80197198`, then emits `G_DL 0x8022AB68` (combiner SHADE,
// alpha blend, smooth shading), four `VTX n=4` + `TRI2` border quads and one
// `VTX n=8` + `TRI2` centre quad. In the intro movie the outer vertices are at
// x = +-160, y = +-108 with alpha 200 and the inner ones at x = -75..85,
// y = -55..65 with alpha 10 (read from RDRAM, 2026-11-01). The projection maps
// x = +-160 to the 4:3 picture's left and right edges.
//
// RT64's Expand counter-scales that projection, so the ring keeps its 4:3 size:
// the picture is darkened up to the 4:3 border and the extension next to it is
// not, which shows as a dark vertical line on each side. With WIDESCREEN on, the
// hook in config/banks/config-bankL.toml scales the copied vertices' x by
// (160 + e) / 160 after the copy, so the ring spans the wide picture with the
// same proportions. The cutscene's own data is not touched, and the hook is a
// no-op while the extension is 0.

#include "widescreen_gbi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace ogre::wsgbi;

namespace {

// The frame's vertex allocation pointer; `func_ovlL_8022BE50` copies the ring
// to it and advances it by 0x3A0 after the draw.
constexpr uint32_t kVertexPointer = 0x80197198u;
// F3DEX2 vertices: s16 x, y, z, u16 flag, s16 s, t, u8 rgba.
constexpr uint32_t kVertexBytes = 16;
// The draw loads vertices 0..9 (four VTX n=4 at 2-vertex steps, one VTX n=8).
constexpr int kRingVertices = 10;
// Half the native picture width in the ring's model units.
constexpr double kHalfWidth = 160.0;

}  // namespace

// func_ovlL_8022BE50 at 0x8022BE80, after the copy of the ring's vertices.
extern "C" void ogre_cutws_vignette(uint8_t* rdram) {
    const int e = ogre::widescreen_2d_extension();
    if (e <= 0) {
        return;
    }
    const uint32_t vertices = read32(rdram, kVertexPointer);
    if (!in_ram(vertices, kRingVertices * kVertexBytes)) {
        return;
    }
    const double scale = (kHalfWidth + e) / kHalfWidth;
    for (int i = 0; i < kRingVertices; ++i) {
        const uint32_t x_at = vertices + static_cast<uint32_t>(i) * kVertexBytes;
        const double x = read_s16(rdram, x_at) * scale;
        write_s16(rdram, x_at, static_cast<int16_t>(std::clamp(std::lround(x), -32768L, 32767L)));
    }
    static int logged_extension = -1;
    if (logged_extension != e) {
        logged_extension = e;
        std::fprintf(stderr, "[cutws] vignette ring widened for %d px per side\n", e);
        std::fflush(stderr);
    }
}
