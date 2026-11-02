#pragma once

// Shared pieces of the patched 2D widescreen screens (map_widescreen.cpp,
// organize_widescreen.cpp): guest-memory access, a writer that appends to the
// game's display list, and RT64 extended-GBI encoders. See
// docs/guides/widescreen.md.
//
// Coordinates follow RT64's extended GBI (tools/RT64/include/rt64_extended_gbi.h,
// `RDP::movedFromOrigin`, `convertFixedRect`): a LEFT-origin x is the native x
// measured from the left edge of the wide picture, a RIGHT-origin x is written
// as `x - 320` and measured from the right edge, and a NONE-origin x stays
// centred with the 320-wide picture. A rectangle whose two edges use different
// origins stretches its texture across the extra width.

#include <cstdint>
#include <cstring>

namespace ogre {

// The width the picture gains on each side while RT64's Expand is on, in
// 320-wide framebuffer pixels, rounded up. 0 while the scene is drawn 4:3,
// which turns every hook off. Safe to call from any thread.
int widescreen_2d_extension();

// Published once per frame by `widescreen_update` (main thread).
void widescreen_2d_set_extension(int pixels);

namespace wsgbi {

constexpr uint32_t kKseg0 = 0x80000000u;
constexpr uint32_t kKseg0End = 0x80800000u;

// gDisplayListHead. Every emitter appends at `*(u32*)0x800E9BA0`.
constexpr uint32_t kDlHead = 0x800E9BA0u;

constexpr uint32_t kExOp = 0x64u << 24;
constexpr uint32_t kExTexRect = 0x02u;
constexpr uint32_t kExFillRect = 0x03u;
constexpr uint32_t kExScissor = 0x05u;
constexpr uint32_t kExRectAlign = 0x06u;
constexpr uint32_t kExViewportAlign = 0x07u;
constexpr uint32_t kExScissorAlign = 0x08u;
constexpr uint32_t kExForceUpscale2D = 0x0Eu;
constexpr uint32_t kExPushScissor = 0x17u;
constexpr uint32_t kExPopScissor = 0x18u;
constexpr uint32_t kExPushOtherMode = 0x19u;
constexpr uint32_t kExPopOtherMode = 0x1Au;
constexpr uint32_t kOriginLeft = 0x000u;
constexpr uint32_t kOriginRight = 0x400u;
constexpr uint32_t kOriginNone = 0x800u;
// RDP::movedFromOrigin adds the colour image's width to a RIGHT-origin x.
constexpr int32_t kRightShift = 320 * 4;
// gEXEnable: RT64's hook on F3DEX2's G_SPNOOP (0xE0), magic 0x525464, op
// ENABLE, extended opcode 0x64.
constexpr uint32_t kEnableW0 = 0xE0525464u;
constexpr uint32_t kEnableW1 = 0x10000064u;

inline bool in_ram(uint32_t guest, uint32_t bytes) {
    return guest >= kKseg0 && guest < kKseg0End && bytes <= kKseg0End - guest;
}

// Guest memory, as `recomp.h` lays it out: a word is a native u32 at
// `guest - 0x80000000`, a halfword a native u16 at that offset `^ 2`.
inline uint32_t read32(const uint8_t* rdram, uint32_t guest) {
    uint32_t value = 0;
    std::memcpy(&value, rdram + (guest - kKseg0), sizeof(value));
    return value;
}

inline void write32(uint8_t* rdram, uint32_t guest, uint32_t value) {
    std::memcpy(rdram + (guest - kKseg0), &value, sizeof(value));
}

inline int16_t read_s16(const uint8_t* rdram, uint32_t guest) {
    uint16_t value = 0;
    std::memcpy(&value, rdram + ((guest - kKseg0) ^ 2u), sizeof(value));
    return static_cast<int16_t>(value);
}

inline void write_s16(uint8_t* rdram, uint32_t guest, int16_t value) {
    std::memcpy(rdram + ((guest - kKseg0) ^ 2u), &value, sizeof(value));
}

inline uint32_t pack16(int32_t high, int32_t low) {
    return ((static_cast<uint32_t>(high) & 0xFFFFu) << 16) | (static_cast<uint32_t>(low) & 0xFFFFu);
}

// A 10.2 x for an origin: RIGHT-origin values are offsets from x = 320.
inline int32_t x_for(uint32_t origin, int32_t x_quarters) {
    return origin == kOriginRight ? x_quarters - kRightShift : x_quarters;
}

inline uint32_t dl_head(const uint8_t* rdram) {
    return read32(rdram, kDlHead);
}

inline void set_dl_head(uint8_t* rdram, uint32_t head) {
    write32(rdram, kDlHead, head);
}

// Writes commands at a guest address.
struct Writer {
    uint8_t* rdram;
    uint32_t at;

    void cmd(uint32_t w0, uint32_t w1) {
        write32(rdram, at, w0);
        write32(rdram, at + 4, w1);
        at += 8;
    }
    void pipe_sync() { cmd(0xE7000000u, 0); }
    void enable() { cmd(kEnableW0, kEnableW1); }
    // gEXTextureRectangle. Coordinates are 10.2, s/t are 10.5, dsdx/dtdy are
    // s5.10 (0x400 is 1:1).
    void tex_rect(uint32_t tile, uint32_t lorigin, uint32_t rorigin, int32_t ulx, int32_t uly,
                  int32_t lrx, int32_t lry, int32_t s, int32_t t, int32_t dsdx = 0x400,
                  int32_t dtdy = 0x400) {
        cmd(kExOp | kExTexRect, (tile & 7u) | ((lorigin & 0xFFFu) << 3) | ((rorigin & 0xFFFu) << 15));
        cmd(pack16(x_for(lorigin, ulx), uly), pack16(x_for(rorigin, lrx), lry));
        cmd(pack16(s, t), pack16(dsdx, dtdy));
    }
    // gEXForceUpscale2D: later rectangles sample at the output resolution
    // instead of snapping to the native pixel (the app sets RT64's Upscale2D
    // to Original, renderer.cpp). A rectangle stretched by a non-integer
    // factor then shows no doubled texel rows.
    void force_upscale_2d(bool force) { cmd(kExOp | kExForceUpscale2D, force ? 1u : 0u); }
    // gEXSetScissor, in pixels.
    void scissor(uint32_t lorigin, uint32_t rorigin, int ulx, int uly, int lrx, int lry) {
        cmd(kExOp | kExScissor, (lorigin << 2) | (rorigin << 14));
        cmd(pack16(x_for(lorigin, ulx * 4), uly * 4), pack16(x_for(rorigin, lrx * 4), lry * 4));
    }
    // A scissor over the whole wide picture with the game's own full-screen
    // extent, (0,0)-(319,239) (`ED000000 004FC3BC`). RT64 counter-scales a
    // projection only when its scissor spans the frame pair's merged scissor
    // (`ProjectionProcessor::processScene`, `coversWholeWidth`), so a scissor
    // that reaches x = 320 stops the game's own full-screen projections from
    // widening: the sides then show whatever an older frame left there.
    void scissor_full() { scissor(kOriginLeft, kOriginRight, 0, 0, 319, 239); }
    // gEXSetScissorAlign: later G_SETSCISSORs take these origins and keep their
    // native values; the bounds are the 320x240 picture.
    void scissor_align(uint32_t lorigin, uint32_t rorigin) {
        const int32_t left = lorigin == kOriginRight ? -kRightShift : 0;
        const int32_t right = rorigin == kOriginRight ? -kRightShift : 0;
        cmd(kExOp | kExScissorAlign, lorigin | (rorigin << 12));
        cmd(pack16(left, 0), pack16(right, 0));
        cmd(pack16(0, 0), pack16(320 * 4, 240 * 4));
    }
    // The default scissor anchoring: no origin, no bounds.
    void scissor_align_reset() {
        cmd(kExOp | kExScissorAlign, kOriginNone | (kOriginNone << 12));
        cmd(0, 0);
        cmd(pack16(0, 0), pack16(0xFFFF, 0xFFFF));
    }
    // gEXSetRectAlign: later G_TEXRECTs take these origins, with the same
    // offset rule. (NONE, NONE) is the default. `dx`/`dy` (pixels) move every
    // later rectangle by that much (`RDP::drawRect` adds the offsets before it
    // applies the origins).
    void rect_align(uint32_t lorigin, uint32_t rorigin, int dx = 0, int dy = 0) {
        const int32_t left = (lorigin == kOriginRight ? -kRightShift : 0) + dx * 4;
        const int32_t right = (rorigin == kOriginRight ? -kRightShift : 0) + dx * 4;
        cmd(kExOp | kExRectAlign, lorigin | (rorigin << 12));
        cmd(pack16(left, dy * 4), pack16(right, dy * 4));
    }
    // gEXSetViewportAlign: later viewports (G_MOVEMEM G_MV_VIEWPORT) take this
    // origin (`RSP::setViewport` moves the x translation from it), and RT64
    // does not counter-scale a projection whose viewport has one, so triangles
    // under it keep their native layout measured from that edge.
    void viewport_align(uint32_t origin) {
        const int32_t offset = origin == kOriginRight ? -kRightShift : 0;
        cmd(kExOp | kExViewportAlign, origin);
        cmd(pack16(offset, 0), 0);
    }
    // G_MOVEMEM G_MV_VIEWPORT (F3DEX2): load the viewport at `address`.
    void viewport(uint32_t address) { cmd(0xDC080008u, address); }
};

}  // namespace wsgbi
}  // namespace ogre
