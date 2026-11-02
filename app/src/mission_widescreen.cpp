// The mission field's cursor (scene 0x03) in widescreen.
//
// The field is 3D, and RT64's Expand counter-scales its projection, which keeps
// every world point at the same 320-wide x and shows more field at the sides
// (x < 0 and x > 320). The field cursor is two f32 screen pixels at state+0x08
// / +0x0C (`state = *(u32*)0x801AB870`), moved by `func_ovlN_801BE4E0`. The
// mover adds the D-pad delta, applies the field's world limits, and clamps x to
// [40, 280] (0x801BEB7C..0x801BEBB8) and y to [30, 210]. At 0x801BEC0C it
// compares the clamped position with the moved one: when the clamp changed it,
// the delta bytes (state+0x06/+0x07) stay set and `func_ovlN_801BECF0` pans the
// camera by them; otherwise they are cleared. The clamp is therefore also the
// point where panning starts.
//
// With WIDESCREEN on, the x clamp becomes [40 - e, 280 + e], so the cursor
// reaches the wide picture's edges before the camera pans. Two hooks in
// config/banks/config-bankN.toml: one before the clamp saves x, one after it
// re-clamps the saved x to the wide bounds. Both are no-ops while the extension
// is 0.
//
// The cursor is drawn by `func_ovlN_801BBA40(x, y)` through the game's 2D
// sprite blitter `func_80176C8C`, which clips every rectangle to the 320x240
// picture (0x80176E20..0x80176EDC): a sprite left of x = 0 is cut and one past
// x = 320 is clamped or dropped, so the cursor vanished once it crossed the 4:3
// edge (developer report, 2026-11-01). While the cursor draw runs in
// widescreen, a rectangle that crosses the 320x240 edge is written as an
// extended-GBI rectangle with signed coordinates instead, and the blitter's
// own clipped copy is skipped. Every other caller of the blitter keeps the
// game's clipping.

#include "widescreen_gbi.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace ogre::wsgbi;

namespace {

constexpr uint32_t kStatePointer = 0x801AB870u;
constexpr uint32_t kCursorX = 0x08u;  // f32
constexpr float kCursorMinX = 40.0f;
constexpr float kCursorMaxX = 280.0f;

float g_saved_x = 0.0f;
bool g_have_saved_x = false;

// True while `func_ovlN_801BBA40` (the cursor draw) runs with the picture wide.
bool g_cursor_draw = false;

// The blitter's clip limits, in 10.2: the 320x240 picture.
constexpr int32_t kPictureRight = 320 * 4;
constexpr int32_t kPictureBottom = 240 * 4;

uint32_t cursor_state(const uint8_t* rdram) {
    const uint32_t state = read32(rdram, kStatePointer);
    return in_ram(state, kCursorX + 4) ? state : 0;
}

float read_f32(const uint8_t* rdram, uint32_t guest) {
    const uint32_t bits = read32(rdram, guest);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void write_f32(uint8_t* rdram, uint32_t guest, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    write32(rdram, guest, bits);
}

}  // namespace

// func_ovlN_801BE4E0 at 0x801BEB74, before the screen clamp: the x it is about
// to clamp.
extern "C" void ogre_msws_cursor_pre(uint8_t* rdram) {
    g_have_saved_x = false;
    if (ogre::widescreen_2d_extension() <= 0) {
        return;
    }
    const uint32_t state = cursor_state(rdram);
    if (state == 0) {
        return;
    }
    g_saved_x = read_f32(rdram, state + kCursorX);
    g_have_saved_x = true;
}

// func_ovlN_801BE4E0 at 0x801BEBC4, after the x clamp: the saved x clamped to
// the wide bounds instead.
extern "C" void ogre_msws_cursor_post(uint8_t* rdram) {
    if (!g_have_saved_x) {
        return;
    }
    g_have_saved_x = false;
    const int e = ogre::widescreen_2d_extension();
    const uint32_t state = cursor_state(rdram);
    if (e <= 0 || state == 0) {
        return;
    }
    const float extension = static_cast<float>(e);
    write_f32(rdram, state + kCursorX,
              std::clamp(g_saved_x, kCursorMinX - extension, kCursorMaxX + extension));
}

// func_ovlN_801BBA40, at its entry (0x801BBA40) and at its epilogue
// (0x801BCD00), which every path out of it reaches.
extern "C" void ogre_msws_cursor_draw_begin() {
    g_cursor_draw = ogre::widescreen_2d_extension() > 0;
}

extern "C" void ogre_msws_cursor_draw_end() {
    g_cursor_draw = false;
}

// func_80176C8C at 0x80176E20, where the blitter has the rectangle it is about
// to clip: `ulx`/`uly`/`lrx`/`lry` in 10.2 ($t3, $t4, $t1, $t2), `s`/`t` in 10.5
// ($a3, $t0), the s5.10 steps `dsdx`/`dtdy` ($a2, $t5) and the tile ($t7).
// During the cursor draw in widescreen, a rectangle that crosses the 320x240
// edge is appended unclipped and 1 is returned; the hook then sets $t3 to 1280
// so the blitter's own test (0x80176E38, ulx < 1280) drops its copy. Returns 0
// to leave the rectangle to the game.
//
// The game's scissor is not enough for it: RT64 maps a scissor with no edge
// origin onto the centred 4:3 picture for a rectangle that does not span the
// whole width (`invRatioScale` in rt64_framebuffer_renderer.cpp), so the
// unclipped rectangle was still cut at the 4:3 edge (developer report,
// 2026-11-01). The rectangle is drawn between gEXPushScissor and
// gEXPopScissor, under the game's full-screen extent (0,0)-(319,239) anchored
// to both edges of the wide picture (`Writer::scissor_full`). RT64 merges every
// draw's scissor into the frame pair's, rectangles included (`RDP::drawRect`),
// and a first build that used (0,0)-(320,240) widened it past the field's own:
// the field's projection stopped qualifying for Expand and the picture fell
// back to 4:3 whenever the cursor crossed the edge (developer report,
// 2026-11-01).
extern "C" int32_t ogre_msws_blit(uint8_t* rdram, int32_t ulx, int32_t uly, int32_t lrx,
                                  int32_t lry, int32_t s, int32_t t, int32_t dsdx, int32_t dtdy,
                                  int32_t tile) {
    if (!g_cursor_draw) {
        return 0;
    }
    if (ulx >= 0 && uly >= 0 && lrx <= kPictureRight && lry <= kPictureBottom) {
        return 0;
    }
    if (lrx <= ulx || lry <= uly || ulx < -0x8000 || lrx > 0x7FFF || uly < -0x8000 ||
        lry > 0x7FFF) {
        return 0;
    }
    const uint32_t head = dl_head(rdram);
    if (!in_ram(head, 96)) {
        return 0;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::fprintf(stderr, "[msws] field cursor drawn past the 4:3 edge (rect x %d..%d)\n",
                     ulx / 4, lrx / 4);
        std::fflush(stderr);
    }
    Writer w{rdram, head};
    w.pipe_sync();
    w.enable();
    w.cmd(kExOp | kExPushScissor, 0);
    w.scissor_full();
    w.tex_rect(static_cast<uint32_t>(tile) & 7u, kOriginNone, kOriginNone, ulx, uly, lrx, lry,
               s, t, dsdx, dtdy);
    w.pipe_sync();
    w.cmd(kExOp | kExPopScissor, 0);
    set_dl_head(rdram, w.at);
    return 1;
}
