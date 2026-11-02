// The Organize Screen (scene 0x06) in widescreen.
//
// The screen draws with an orthographic projection laid out for 320x240
// (`guOrtho(-160, 160, -120, 120, -10, 100, 1)`, the call at 0x801B80BC in
// `func_ovlS_801B7FC0`), so RT64's Expand keeps every draw at its 4:3 size and
// position, centred, and the sides stay black. With WIDESCREEN on, the module's
// draw is patched (config/banks/config-bankS.toml):
//
// - the cobblestone background is one quad of exactly 320x240 (the sub-list
//   `0x06002528`, vertices at segment 6 + 0x24E8 = the screen asset
//   `state[0]` + 0x24E8). Its vertices are widened in place to x = +-288 with
//   the texture coordinates extended at the same rate, so it fills any picture
//   up to 1.8 times as wide as 4:3; the scissor trims it. They are put back
//   while WIDESCREEN is off;
// - the windows are drawn by one loop over 10 slots at `state + 1616 + 264*i`
//   (0x801BA1B8..0x801BA594): the counters (content `0x801B111C`) are anchored
//   to the left edge, WAR FUNDS (`0x801B1644`) to the nearer edge, and the name
//   plate (`0x801B2000`) to the right edge. Every other window (pop-up menus and
//   the sub-screens' windows) stays centred;
// - the tile grid and the units are centred, as in 4:3.
//
// The developer chose this layout on 2026-11-01. docs/guides/widescreen.md has
// the method.

#include "widescreen_gbi.hpp"

#include <cstdint>
#include <cstdio>

using namespace ogre::wsgbi;

namespace {

// The Organize state object, a heap pointer at 0x80196B18; `state[0]` is the
// screen asset the display list uses as segment 6 (0x801B8074).
constexpr uint32_t kStatePointer = 0x80196B18u;
constexpr uint32_t kStateBytes = 1616 + 10 * 264;

// The background quad: four F3DEX2 vertices (16 bytes: s16 x, y, z, u16 flag,
// s16 s, t, rgba) at asset + 0x24E8, drawn by `G_DL 0x06002528` at 0x801B8264.
constexpr uint32_t kBackgroundVertices = 0x24E8u;
struct QuadVertex {
    int16_t x, y, s, t;
};
// As loaded (read from RDRAM on this screen): x = +-160, y = +-120. The tile is
// a 64x64 CI4 with masks 6 and TEXTURE scale 0.5, so 4096 in s is one period.
constexpr QuadVertex kOriginal[4] = {
    {-160, -120, 28608, 21440}, {-160, 120, 28608, 0}, {160, 120, 0, 0}, {160, -120, 0, 21440}};
// Widened by 128 on each side; s moves 28608/320 per pixel, and both ends are
// shifted down by 16384 (four periods) to stay inside s16.
constexpr QuadVertex kWide[4] = {
    {-288, -120, 23667, 21440}, {-288, 120, 23667, 0}, {288, 120, -27827, 0}, {288, -120, -27827, 21440}};

// The window slots (func_ovlS_801CC994 builds them): +0 content callback,
// +12/+16 left/top, +20/+24 right/bottom, +230 active.
constexpr uint32_t kWindowSlots = 1616;
constexpr uint32_t kWindowStride = 264;
constexpr uint32_t kWindowContent = 0x00;
constexpr uint32_t kWindowLeft = 0x0C;
constexpr uint32_t kWindowRight = 0x14;
// The viewport every draw on this screen uses (G_MOVEMEM DC080008 in the common
// setup list at 0x801869E8). A window's parchment fill is triangles under the
// ortho projection, so an anchored window re-issues it under its edge's
// viewport anchoring.
constexpr uint32_t kViewport = 0x80186330u;
// Content callbacks of the panels the developer anchored (window creation at
// 0x801CC23C, 0x801CC280 and 0x801CC374).
constexpr uint32_t kCountersContent = 0x801B111Cu;
constexpr uint32_t kWarFundsContent = 0x801B1644u;
constexpr uint32_t kNamePlateContent = 0x801B2000u;

// Game-thread state for the draw in progress.
bool g_wide = false;
bool g_window_aligned = false;

uint32_t organize_state(const uint8_t* rdram) {
    const uint32_t state = read32(rdram, kStatePointer);
    return in_ram(state, kStateBytes) ? state : 0;
}

bool quad_matches(const uint8_t* rdram, uint32_t vertices, const QuadVertex (&want)[4]) {
    for (int i = 0; i < 4; ++i) {
        const uint32_t v = vertices + i * 16u;
        if (read_s16(rdram, v + 0) != want[i].x || read_s16(rdram, v + 2) != want[i].y ||
            read_s16(rdram, v + 8) != want[i].s || read_s16(rdram, v + 10) != want[i].t) {
            return false;
        }
    }
    return true;
}

void write_quad(uint8_t* rdram, uint32_t vertices, const QuadVertex (&values)[4]) {
    for (int i = 0; i < 4; ++i) {
        const uint32_t v = vertices + i * 16u;
        write_s16(rdram, v + 0, values[i].x);
        write_s16(rdram, v + 8, values[i].s);
    }
}

// The edge a window is anchored to, or NONE to leave it centred.
uint32_t window_origin(const uint8_t* rdram, uint32_t window) {
    const uint32_t content = read32(rdram, window + kWindowContent);
    if (content == kCountersContent) {
        return kOriginLeft;
    }
    if (content == kNamePlateContent) {
        return kOriginRight;
    }
    if (content == kWarFundsContent) {
        // The Organize Screen's WAR FUNDS sits at the left; a sub-screen
        // creates the same window at the right (0x801DF0A4).
        const int32_t left = static_cast<int32_t>(read32(rdram, window + kWindowLeft));
        const int32_t right = static_cast<int32_t>(read32(rdram, window + kWindowRight));
        return (left + right) / 2 < 160 ? kOriginLeft : kOriginRight;
    }
    return kOriginNone;
}

}  // namespace

// func_ovlS_801B7FC0 at 0x801B8014, after its "screen is up" checks and before
// it writes to the display list. Widens or restores the background quad and,
// while wide, enables RT64's extended GBI for the window hooks.
extern "C" void ogre_orgws_begin(uint8_t* rdram) {
    g_window_aligned = false;
    g_wide = ogre::widescreen_2d_extension() > 0;
    const uint32_t state = organize_state(rdram);
    if (state == 0) {
        g_wide = false;
        return;
    }
    const uint32_t asset = read32(rdram, state);
    const uint32_t vertices = asset + kBackgroundVertices;
    if (in_ram(vertices, 64)) {
        if (g_wide && quad_matches(rdram, vertices, kOriginal)) {
            write_quad(rdram, vertices, kWide);
        }
        else if (!g_wide && quad_matches(rdram, vertices, kWide)) {
            write_quad(rdram, vertices, kOriginal);
        }
    }
    if (!g_wide) {
        return;
    }
    const uint32_t head = dl_head(rdram);
    if (!in_ram(head, 8)) {
        g_wide = false;
        return;
    }
    Writer w{rdram, head};
    w.enable();
    set_dl_head(rdram, w.at);
}

// The window loop at 0x801BA1D0, for an active window (`$s0`), before its frame
// is drawn. An anchored window gets a scissor over the whole wide picture (its
// frame is drawn before the loop sets the window's own scissor) and the
// rectangle and scissor anchoring of its edge.
extern "C" void ogre_orgws_window(uint8_t* rdram, uint32_t window) {
    if (!g_wide || !in_ram(window, kWindowStride)) {
        return;
    }
    const uint32_t origin = window_origin(rdram, window);
    if (origin == kOriginNone) {
        return;
    }
    const uint32_t head = dl_head(rdram);
    if (!in_ram(head, 64)) {
        return;
    }
    Writer w{rdram, head};
    w.scissor_full();
    w.rect_align(origin, origin);
    w.scissor_align(origin, origin);
    w.viewport_align(origin);
    w.viewport(kViewport);
    set_dl_head(rdram, w.at);
    g_window_aligned = true;
}

// The window loop at 0x801BA584, after the window's content (and for inactive
// slots). Puts the default anchoring back, and replaces the full-screen scissor
// the loop just set under the window's anchoring with one over the whole wide
// picture, so the next anchored window's frame is not clipped.
extern "C" void ogre_orgws_window_end(uint8_t* rdram) {
    if (!g_window_aligned) {
        return;
    }
    g_window_aligned = false;
    const uint32_t head = dl_head(rdram);
    if (!in_ram(head, 64)) {
        return;
    }
    Writer w{rdram, head};
    w.rect_align(kOriginNone, kOriginNone);
    w.scissor_align_reset();
    w.scissor_full();
    w.viewport_align(kOriginNone);
    w.viewport(kViewport);
    set_dl_head(rdram, w.at);
}
