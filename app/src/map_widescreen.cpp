// The world map (scene 0x05) in widescreen.
//
// The map is drawn entirely with 2D rectangles, so RT64's Expand aspect ratio
// alone leaves it 4:3 between black bars. With WIDESCREEN on, the map module's
// own draw is patched (config/banks/config-bankM.toml hooks into
// `func_ovlM_801A103C` and its helpers) to emit RT64 extended-GBI commands.
//
// The game draws its stone frame at x 14..302, y 15..238, so even a 4:3
// picture has black margins (14 px left, 18 px right, 15 rows on top, one row
// at the bottom) that a CRT's overscan hid. In widescreen the frame sits on the
// picture's edges and the map fills the window inside it (developer,
// 2026-11-01: "the borders themselves should be expanded so that they are on
// the outer edge, with the map in the middle"):
//
// - the frame is split at texture column 120 into a left part anchored at
//   x = 0 and a right part that ends at the right edge (the bottom bar's pillar
//   stays beside the MONTH/DATE panel); the gap in the top and bottom bars is
//   filled by repeating 64 texel columns of the bars;
// - the top bar moves up to y = 0 and the bottom bar down to y = 239, and the
//   pillars between them are stretched to meet both;
// - the map window grows to the frame's new inner edge: extra strips of the
//   512x320 map image left, right and above the game's strips, under a scissor
//   anchored to the screen edges;
// - the frame's inner-shadow layer follows the frame (its middle columns are
//   uniform, so they are stretched across the gap);
// - the location plate moves with the frame's top-left corner and the
//   MONTH/DATE panel with its bottom-right;
// - sprites and location icons are drawn with signed coordinates, so the ones
//   in the extensions are not clipped at x = 0;
// - the scroll is clamped so the larger window never runs past the map image,
//   and the cursor's bounds keep the game's margins inside the larger window.
//
// Every function here runs on the game thread, from a hook inside the map
// module's generated code, and appends to or rewrites the display list the game
// is building. The extension is read once per map draw (`ogre_mapws_begin`), so
// one frame's commands agree with each other. All of it is off (the game's own
// code runs unchanged) while the extension is 0. docs/guides/widescreen.md has
// the method and the reasons for each number.

#include "map_cursor.hpp"
#include "widescreen_gbi.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

using namespace ogre::wsgbi;

namespace {

// The map state object, a heap pointer at 0x80197B18 (docs/scenes.md).
constexpr uint32_t kStatePointer = 0x80197B18u;
constexpr uint32_t kStateMapAsset = 0x00u;  // u32: a 512-byte TLUT, then the CI8 map image
constexpr uint32_t kStateScrollX = 0x40u;   // s32: the map column drawn at screen x = 32
constexpr uint32_t kStateScrollY = 0x44u;   // s32: the map row drawn at screen y = 32
constexpr uint32_t kStateCursorX = 0x52u;   // s16
constexpr uint32_t kStateBytes = 0x60u;

// How far the wide layout moves the frame: its left edge from x 14 to 0, its
// right edge from x 302 to 320, its top from y 15 to 0, and its last opaque
// row (texture row 223) from y 238 to 239. The map window inside the frame
// grows by the same amounts.
constexpr int kMoveLeft = 14;
constexpr int kMoveRight = 18;
constexpr int kMoveUp = 15;
constexpr int kMoveDown = 1;

// The terrain loop in func_ovlM_801A103C (0x801A16BC..0x801A185C): 25 strips
// from a 512x320 CI8 image (the heap block before it is 0x28200 bytes: the
// TLUT and 512 * 320), at y 32..232 under the scissor (32,32)-(288,217). Each
// strip loads 256 texels but its rectangle is x 32..287 (lrx 0x47C), so it
// shows 255 columns; column 287 is under the frame in 4:3, and the right
// extension starts there.
constexpr int kMapWidth = 512;
constexpr int kTerrainLeft = 32;
constexpr int kTerrainRight = 288;
constexpr int kTerrainDrawnRight = 287;
constexpr int kTerrainTop = 32;
constexpr int kTerrainBottom = 217;
constexpr int kTerrainStrips = 25;
constexpr int kStripRows = 8;
// A CI8 load shares TMEM with its 256-entry TLUT, so one load holds at most
// 2048 bytes: 256 texels by 8 rows.
constexpr int kMaxLoadColumns = 256;
// The game clamps the scroll to [0, 256] x [0, 124] (0x8019B638..0x8019B684)
// and the cursor to [40, 272] x [40, 192] (0x8019B374, 0x8019B39C, 0x8019B3D4,
// 0x8019B49C..0x8019B4D8, 0x8019B8D4..0x8019B91C).
constexpr int kScrollMaxX = kMapWidth - (kTerrainRight - kTerrainLeft);
constexpr int kScrollMaxY = 124;
constexpr int kCursorMinX = 40;
constexpr int kCursorMaxX = 272;
constexpr int kCursorMinY = 40;
constexpr int kCursorMaxY = 192;
// Side strips are drawn this much wider than the extension and clipped by the
// scissor, so rounding in RT64's own extension never leaves a gap.
constexpr int kStripSlack = 2;

// The shading layer's loop (0x801A19D8..0x801A1B18): 24 blocks of 96 bytes,
// each one 256x8 I8 strip drawn at x 32..287, y 32 + 8 * block. It is the
// frame's inner shadow, with the MONTH/DATE panel's shadow at rows ~169..185.
constexpr int kVignetteBlocks = 24;
constexpr uint32_t kVignetteBlockBytes = 96;
constexpr int kVignetteRows = 8;
// Its columns 64..104 are the same value across every row (measured on the
// layer at 0x8022F550: spread 0), so they stretch without a seam. The MONTH/DATE
// panel's shadow starts at column ~108, inside the right-anchored part.
constexpr int kVignetteSplitLeft = 64;
constexpr int kVignetteSplitRight = 104;

// The frame's loop (0x801A1D64..0x801A1EE0): 38 blocks of 96 bytes, each one
// 288x6 CI4 strip drawn at x 14..302, y 15 + 6 * block. In the texture
// (state+0x38, a 16-colour TLUT then the image) rows 0..16 are the top bar
// and row 17 is the window's top edge; the bottom bar is tall from column 0 to
// a small pillar at columns 128..134 (from row ~197), and lower from 135 on,
// with a transparent cut-out above it that the MONTH/DATE panel covers. Rows
// 224..227 are transparent.
constexpr int kFrameBlocks = 38;
constexpr uint32_t kFrameBlockBytes = 96;
constexpr int kFrameRows = 6;
constexpr int kFrameTop = 15;
constexpr int kFrameWidth = 288;
// The split is left of the pillar, so the pillar stays beside the panel.
constexpr int kFrameSplitColumn = 120;
// Rows 0..17 move up with the top bar and rows 186..227 down with the bottom
// bar (the panel, which covers rows ~188..217, moves with them). The pillar
// rows between are stretched over the space that leaves.
constexpr int kFrameTopRows = 18;
constexpr int kFrameBottomRow = 186;
// The bars' gap is filled from texture columns 48..111, repeated: plain stone in
// the top bar and the tall bottom bar, transparent in the side rows. A tile with
// masks = 6 at TMEM word 3 (column 48 of a CI4 row is byte 24). A 32-column
// window repeated its crack pattern three times across a 16:9 gap.
constexpr uint32_t kFillerTmemWord = 3;

// The location plate (func_ovlM_801A26C0 at 24,22) overlaps the frame's top-left
// corner and the MONTH/DATE panel (func_ovlM_801A2410 at 150,203) sits in the
// bottom bar's cut-out; each moves with its corner of the frame.
constexpr int kPlateDx = -kMoveLeft;
constexpr int kPlateDy = -kMoveUp;
constexpr int kPanelDx = kMoveRight;
constexpr int kPanelDy = kMoveDown;

// Game-thread state for the map draw in progress.
int g_frame_extension = 0;
bool g_in_draw = false;
uint32_t g_sprite_origin = kOriginNone;
int g_sprite_dx = 0;
int g_sprite_dy = 0;

// The game's scissor, (0,0)-(319,239). The RDP's lower-right edge is
// exclusive, so the game never draws its last column and row; the wide layout
// puts the frame and the fade there, under a scissor of its own.
constexpr uint32_t kScissorGameW1 = 0x004FC3BCu;
// How far past the right edge the frame's right part and the fade are drawn
// (see `ogre_mapws_frame`).
constexpr int kEdgeBleed = 2;

// When the last wide map draw began (steady clock, ms), for the presenter's
// overscan crop (`ogre_overscan_crop_override`, read on RT64's present thread).
std::atomic<int64_t> g_last_wide_draw_ms{INT64_MIN / 2};
// The map draws a frame every 33 or 50 ms; a present within this long of the
// last wide draw shows the map.
constexpr int64_t kWideDrawHoldMs = 100;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void log_once(const char* what) {
    static const char* last = nullptr;
    if (last != what) {
        last = what;
        std::fprintf(stderr, "[mapws] %s\n", what);
        std::fflush(stderr);
    }
}

// The map state, or 0 when the pointer does not name RAM.
uint32_t map_state(const uint8_t* rdram) {
    const uint32_t state = read32(rdram, kStatePointer);
    return in_ram(state, kStateBytes) ? state : 0;
}

// The screen y (10.2) of frame texture row `row` in the wide layout. Rows up to
// kFrameTopRows move up by kMoveUp, rows from kFrameBottomRow move down by
// kMoveDown, and the rows between are spread evenly over the space between.
int32_t frame_row_y(int row) {
    if (row <= kFrameTopRows) {
        return (kFrameTop - kMoveUp + row) * 4;
    }
    if (row >= kFrameBottomRow) {
        return (kFrameTop + kMoveDown + row) * 4;
    }
    const int32_t y0 = (kFrameTop - kMoveUp + kFrameTopRows) * 4;
    const int32_t y1 = (kFrameTop + kMoveDown + kFrameBottomRow) * 4;
    const int32_t span = kFrameBottomRow - kFrameTopRows;
    return y0 + ((y1 - y0) * (row - kFrameTopRows) + span / 2) / span;
}

// The s5.10 step that spreads `rows` texture rows over `height` quarter pixels.
int32_t dtdy_for(int rows, int32_t height) {
    return height > 0 ? (rows * 4 * 1024 + height / 2) / height : 0x400;
}

// The map window in the wide layout, in the coordinates its origin uses: the
// left edge LEFT-anchored, the right edge RIGHT-anchored.
constexpr int kWindowLeft = kTerrainLeft - kMoveLeft;       // 18
constexpr int kWindowRight = kTerrainRight + kMoveRight;    // 306
constexpr int kWindowTop = kTerrainTop - kMoveUp;           // 17
constexpr int kWindowBottom = kTerrainBottom + kMoveDown;   // 218

// One terrain piece: map columns [c0, c1) of `rows` rows from `map_row`, drawn
// from screen x `x0` at `screen_y`, with the game's own load sequence
// (0x801A16E0..0x801A1858).
void terrain_piece(Writer& w, uint32_t image, int c0, int c1, int map_row, int rows, int screen_y,
                   int32_t x0) {
    if (c1 <= c0 || rows <= 0) {
        return;
    }
    const int width = c1 - c0;
    const uint32_t line = static_cast<uint32_t>((width + 7) / 8);  // 64-bit TMEM words per row
    const int r0 = map_row;
    const int r1 = map_row + rows - 1;
    w.cmd(0xFD4801FFu, image);                                      // SETTIMG CI 8b, 512 wide
    w.cmd(0xF5480000u | (line << 9), 0x07080200u);                  // SETTILE 7, clamp s/t
    w.cmd(0xE6000000u, 0);                                          // LOADSYNC
    w.cmd(0xF4000000u | (static_cast<uint32_t>(c0 * 4) << 12) | static_cast<uint32_t>(r0 * 4),
          0x07000000u | (static_cast<uint32_t>((c1 - 1) * 4) << 12) | static_cast<uint32_t>(r1 * 4));
    w.pipe_sync();
    w.cmd(0xF5480000u | (line << 9), 0x00080200u);                  // SETTILE 0
    w.cmd(0xF2000000u | (static_cast<uint32_t>(c0 * 4) << 12) | static_cast<uint32_t>(r0 * 4),
          (static_cast<uint32_t>((c1 - 1) * 4) << 12) | static_cast<uint32_t>(r1 * 4));
    w.pipe_sync();
    w.tex_rect(0, kOriginNone, kOriginNone, x0 * 4, screen_y * 4, (x0 + width) * 4,
               (screen_y + rows) * 4, c0 * 32, r0 * 32);
    w.pipe_sync();
}

// Map columns [c0, c1) of rows [r0, r0 + rows), split into loads that fit TMEM.
// Column `scroll_x` is at screen x 32, as in the game's strips.
void terrain_band(Writer& w, uint32_t image, int c0, int c1, int map_row, int rows, int screen_y,
                  int scroll_x) {
    for (int c = c0; c < c1; c += kMaxLoadColumns) {
        const int c_end = std::min(c1, c + kMaxLoadColumns);
        terrain_piece(w, image, c, c_end, map_row, rows, screen_y, kTerrainLeft + (c - scroll_x));
    }
}

bool block_is_vignette(const uint8_t* rdram, uint32_t block) {
    return read32(rdram, block) == 0xFD900000u && (read32(rdram, block + 64) >> 24) == 0xE4u &&
           read32(rdram, block + 72) == 0xE1000000u && read32(rdram, block + 80) == 0xF1000000u;
}

bool block_is_frame(const uint8_t* rdram, uint32_t block) {
    return read32(rdram, block) == 0xFD48008Fu && (read32(rdram, block + 64) >> 24) == 0xE4u &&
           read32(rdram, block + 72) == 0xE1000000u && read32(rdram, block + 80) == 0xF1000000u;
}

}  // namespace

// The cursor's play area (map_cursor.hpp). The game's margins inside its
// window are 8 px left and top, 16 px right and 25 px bottom; the wide window
// reaches 14 px further left, 18 px further right and 15 rows higher, and the
// bottom bound keeps the game's value (the window's bottom moved one row).
ogre::MapCursorBounds ogre::map_cursor_bounds() {
    const int e = widescreen_2d_extension();
    if (e <= 0) {
        return {kCursorMinX, kCursorMaxX, kCursorMinY, kCursorMaxY};
    }
    return {kCursorMinX - e - kMoveLeft, kCursorMaxX + e + kMoveRight, kCursorMinY - kMoveUp,
            kCursorMaxY};
}

// --- Hooks: scroll and cursor bounds (func_ovlM_8019AF0C, func_ovlM_801A103C) --

// Clamps the scroll so the wide window, `extension + 14` pixels further left,
// `extension + 18` further right and 15 rows higher than the game's, stays on
// the 512x320 map. Runs after the game's own [0, 256] x [0, 124] clamps and
// before the draw reads the scroll.
extern "C" void ogre_mapws_clamp_scroll(uint8_t* rdram) {
    const int e = ogre::widescreen_2d_extension();
    if (e <= 0) {
        return;
    }
    const uint32_t state = map_state(rdram);
    if (state == 0) {
        return;
    }
    int32_t lo = e + kMoveLeft;
    int32_t hi = kScrollMaxX - e - kMoveRight;
    if (lo > hi) {
        // A picture wider than the map: centre the map in it.
        lo = hi = (lo + hi) / 2;
    }
    const int32_t scroll_x = static_cast<int32_t>(read32(rdram, state + kStateScrollX));
    const int32_t clamped_x = std::clamp(scroll_x, lo, hi);
    if (clamped_x != scroll_x) {
        write32(rdram, state + kStateScrollX, static_cast<uint32_t>(clamped_x));
    }
    const int32_t scroll_y = static_cast<int32_t>(read32(rdram, state + kStateScrollY));
    const int32_t clamped_y = std::clamp(scroll_y, kMoveUp, kScrollMaxY);
    if (clamped_y != scroll_y) {
        write32(rdram, state + kStateScrollY, static_cast<uint32_t>(clamped_y));
    }
}

// The D-pad's left test (0x8019B374, `slti v0, x, 41`): true while the cursor
// is at the left bound.
extern "C" uint64_t ogre_mapws_cursor_at_left(uint64_t original, int32_t x) {
    if (ogre::widescreen_2d_extension() <= 0) {
        return original;
    }
    return static_cast<int16_t>(x) < ogre::map_cursor_bounds().min_x + 1 ? 1 : 0;
}

// The D-pad's right test (0x8019B39C, `slti v0, x, 272`).
extern "C" uint64_t ogre_mapws_cursor_below_right(uint64_t original, int32_t x) {
    if (ogre::widescreen_2d_extension() <= 0) {
        return original;
    }
    return static_cast<int16_t>(x) < ogre::map_cursor_bounds().max_x ? 1 : 0;
}

// The D-pad's up test (0x8019B3D4, `slti v0, y, 41`).
extern "C" uint64_t ogre_mapws_cursor_at_top(uint64_t original, int32_t y) {
    if (ogre::widescreen_2d_extension() <= 0) {
        return original;
    }
    return static_cast<int16_t>(y) < ogre::map_cursor_bounds().min_y + 1 ? 1 : 0;
}

// The stick's x clamp (0x8019B49C..0x8019B4B4): the clamped x the game stores.
extern "C" uint64_t ogre_mapws_cursor_clamp(uint64_t original, int32_t x) {
    if (ogre::widescreen_2d_extension() <= 0) {
        return original;
    }
    const ogre::MapCursorBounds b = ogre::map_cursor_bounds();
    return static_cast<uint64_t>(
        static_cast<int64_t>(std::clamp<int32_t>(static_cast<int16_t>(x), b.min_x, b.max_x)));
}

// The stick's y clamp (0x8019B4C0..0x8019B4D8): the clamped y the game stores.
extern "C" uint64_t ogre_mapws_cursor_clamp_y(uint64_t original, int32_t y) {
    if (ogre::widescreen_2d_extension() <= 0) {
        return original;
    }
    const ogre::MapCursorBounds b = ogre::map_cursor_bounds();
    return static_cast<uint64_t>(
        static_cast<int64_t>(std::clamp<int32_t>(static_cast<int16_t>(y), b.min_y, b.max_y)));
}

// The clamp at 0x8019B8D4..0x8019B8EC, which stores the bound itself: the x it
// read is re-clamped to the wide bounds and stored again.
extern "C" void ogre_mapws_cursor_store(uint8_t* rdram, int32_t x) {
    if (ogre::widescreen_2d_extension() <= 0) {
        return;
    }
    const uint32_t state = map_state(rdram);
    if (state == 0) {
        return;
    }
    const ogre::MapCursorBounds b = ogre::map_cursor_bounds();
    const int32_t clamped = std::clamp<int32_t>(static_cast<int16_t>(x), b.min_x, b.max_x);
    write_s16(rdram, state + kStateCursorX, static_cast<int16_t>(clamped));
}

// The y half of that clamp: `slti v0, y, 40` at 0x8019B8FC, tested at
// 0x8019B900. True sends the game to store the top bound.
extern "C" uint64_t ogre_mapws_cursor_above_top(uint64_t original, int32_t y) {
    if (ogre::widescreen_2d_extension() <= 0) {
        return original;
    }
    return static_cast<int16_t>(y) < ogre::map_cursor_bounds().min_y ? 1 : 0;
}

// 0x8019B908, on that path: `$v0` holds the bound the delay slot at 0x8019B90C
// stores (the game's `li v0, 40` at 0x8019B904).
extern "C" uint64_t ogre_mapws_cursor_top_bound(uint64_t original) {
    if (ogre::widescreen_2d_extension() <= 0) {
        return original;
    }
    return static_cast<uint64_t>(static_cast<int64_t>(ogre::map_cursor_bounds().min_y));
}

// --- Hooks: the map's draw (func_ovlM_801A103C) ------------------------------

// 0x801A1694, after the terrain's setup block and before its strip loop.
// Clears the wide picture, makes later G_SETSCISSORs edge-anchored, and
// replaces the terrain's scissor (32,32)-(288,217) with the wide window's
// (18,17)-(306,218), anchored to the edges.
extern "C" void ogre_mapws_begin(uint8_t* rdram) {
    g_in_draw = false;
    g_sprite_origin = kOriginNone;
    g_sprite_dx = 0;
    g_sprite_dy = 0;
    g_frame_extension = ogre::widescreen_2d_extension();
    if (g_frame_extension <= 0) {
        return;
    }
    const uint32_t head = dl_head(rdram);
    if (!in_ram(head, 256)) {
        return;
    }
    Writer w{rdram, head};
    w.enable();
    w.pipe_sync();
    w.cmd(kExOp | kExPushOtherMode, 0);
    w.cmd(0xE3000A01u, 0x00300000u);  // G_SETOTHERMODE_H: cycle type FILL
    w.cmd(0xF7000000u, 0x00010001u);  // G_SETFILLCOLOR: black, alpha set
    w.scissor_full();
    // gEXFillRectangle over the whole wide picture. RT64's fillRect drops a
    // rectangle whose raw lrx is below its raw ulx, so the right edge is the
    // RIGHT-origin 0 (x = 320) and fill mode's `|= 3` adds the rest; the
    // scissor clips it to the picture.
    w.cmd(kExOp | kExFillRect, kOriginLeft | (kOriginRight << 12));
    w.cmd(pack16(0, 0), pack16(x_for(kOriginRight, 320 * 4), 239 * 4));
    w.pipe_sync();
    w.cmd(kExOp | kExPopOtherMode, 0);
    w.scissor_align(kOriginLeft, kOriginRight);
    // G_SETSCISSOR, mode 0, now edge-anchored.
    w.cmd(0xED000000u | (static_cast<uint32_t>(kWindowLeft * 4) << 12) |
              static_cast<uint32_t>(kWindowTop * 4),
          (static_cast<uint32_t>(kWindowRight * 4) << 12) |
              static_cast<uint32_t>(kWindowBottom * 4));
    set_dl_head(rdram, w.at);
    g_in_draw = true;
    g_last_wide_draw_ms.store(now_ms(), std::memory_order_relaxed);
}

// 0x801A1860, after the terrain's strip loop. Draws the map columns left and
// right of the game's window and the rows above it. Returns the bytes
// appended; the caller adds them to $t3, which the code after the loop stores
// relative to.
extern "C" uint32_t ogre_mapws_terrain(uint8_t* rdram) {
    if (!g_in_draw) {
        return 0;
    }
    const uint32_t state = map_state(rdram);
    const uint32_t head = dl_head(rdram);
    if (state == 0 || !in_ram(head, 8192)) {
        return 0;
    }
    const uint32_t image = read32(rdram, state + kStateMapAsset) + 512;
    const int scroll_x = static_cast<int32_t>(read32(rdram, state + kStateScrollX));
    const int scroll_y = static_cast<int32_t>(read32(rdram, state + kStateScrollY));
    const int left_width =
        std::min(g_frame_extension + kMoveLeft + kStripSlack, kMaxLoadColumns);
    const int right_width =
        std::min(g_frame_extension + kMoveRight + kStripSlack, kMaxLoadColumns);
    const int left0 = std::max(0, scroll_x - left_width);
    const int right0 = scroll_x + (kTerrainDrawnRight - kTerrainLeft);
    const int right1 = std::min(kMapWidth, right0 + right_width);
    Writer w{rdram, head};
    for (int strip = 0; strip < kTerrainStrips; ++strip) {
        const int map_row = scroll_y + strip * kStripRows;
        const int screen_y = kTerrainTop + strip * kStripRows;
        terrain_piece(w, image, left0, scroll_x, map_row, kStripRows, screen_y,
                      kTerrainLeft - (scroll_x - left0));
        terrain_piece(w, image, right0, right1, map_row, kStripRows, screen_y,
                      kTerrainDrawnRight);
    }
    // The rows above the game's window, across its whole width: map rows
    // [scroll_y - 15, scroll_y) at y 17..31. The scroll clamp keeps
    // scroll_y >= 15, so no row above the image is read.
    const int top_row = std::max(0, scroll_y - kMoveUp);
    for (int row = top_row; row < scroll_y; row += kStripRows) {
        const int rows = std::min(kStripRows, scroll_y - row);
        terrain_band(w, image, left0, right1, row, rows, kTerrainTop - (scroll_y - row), scroll_x);
    }
    set_dl_head(rdram, w.at);
    return w.at - head;
}

// 0x801A1B1C, after the shading layer's loop. Rewrites each of the 24 strips
// as a left-anchored part, a stretched middle and a right-anchored part, at the
// height the frame's rows of the same screen position moved to. Returns the
// bytes the list grew by; the caller adds them to $t0.
extern "C" uint32_t ogre_mapws_vignette(uint8_t* rdram) {
    if (!g_in_draw) {
        return 0;
    }
    const uint32_t head = dl_head(rdram);
    const uint32_t start = head - kVignetteBlocks * kVignetteBlockBytes;
    if (!in_ram(start, kVignetteBlocks * 160)) {
        return 0;
    }
    uint32_t blocks[kVignetteBlocks][kVignetteBlockBytes / 4];
    for (int b = 0; b < kVignetteBlocks; ++b) {
        const uint32_t at = start + b * kVignetteBlockBytes;
        if (!block_is_vignette(rdram, at)) {
            log_once("shading layer: unexpected list layout, left 4:3");
            return 0;
        }
        for (uint32_t i = 0; i < kVignetteBlockBytes / 4; ++i) {
            blocks[b][i] = read32(rdram, at + i * 4);
        }
    }
    // Columns 0..63 from the window's left edge, 104..255 up to its right
    // edge, and 64..103 stretched between. The middle's native width is the
    // gap between those two parts, so its step spreads the 40 columns over it.
    const int xl0 = kWindowLeft;
    const int xl1 = kWindowLeft + kVignetteSplitLeft;
    const int xr1 = kWindowRight;
    const int xr0 = kWindowRight - (kTerrainRight - kTerrainLeft - kVignetteSplitRight);
    const int mid_columns = kVignetteSplitRight - kVignetteSplitLeft;
    const int32_t mid_dsdx = (mid_columns * 1024 + (xr0 - xl1) / 2) / (xr0 - xl1);
    Writer w{rdram, start};
    for (int b = 0; b < kVignetteBlocks; ++b) {
        // The load and tile setup (bytes 0..63) are kept as they are.
        for (int i = 0; i < 16; i += 2) {
            w.cmd(blocks[b][i], blocks[b][i + 1]);
        }
        // Shading row v is at the screen row of frame row v + 17 (y 32 + v and
        // 15 + (v + 17) in the game's layout).
        const int frame_row = kTerrainTop - kFrameTop + b * kVignetteRows;
        const int32_t uly = frame_row_y(frame_row);
        const int32_t lry = frame_row_y(frame_row + kVignetteRows);
        const int32_t dtdy = dtdy_for(kVignetteRows, lry - uly);
        const int32_t t = static_cast<int32_t>(blocks[b][19] & 0xFFFFu);
        w.tex_rect(0, kOriginLeft, kOriginLeft, xl0 * 4, uly, xl1 * 4, lry, 0, t, 0x400, dtdy);
        w.tex_rect(0, kOriginLeft, kOriginRight, xl1 * 4, uly, xr0 * 4, lry,
                   kVignetteSplitLeft * 32, t, mid_dsdx, dtdy);
        w.tex_rect(0, kOriginRight, kOriginRight, xr0 * 4, uly, xr1 * 4, lry,
                   kVignetteSplitRight * 32, t, 0x400, dtdy);
        w.pipe_sync();
    }
    set_dl_head(rdram, w.at);
    return w.at - head;
}

// 0x801A1EE4, after the frame's loop. Rewrites each of the 38 strips as a
// repeated filler for the bars' gap, the left part anchored at x = 0 and the
// right part ending at the right edge, at the strip's new height. Returns the
// bytes the list grew by; the caller adds them to $t3.
extern "C" uint32_t ogre_mapws_frame(uint8_t* rdram) {
    if (!g_in_draw) {
        return 0;
    }
    const uint32_t head = dl_head(rdram);
    const uint32_t start = head - kFrameBlocks * kFrameBlockBytes;
    if (!in_ram(start, kFrameBlocks * 192)) {
        return 0;
    }
    uint32_t blocks[kFrameBlocks][kFrameBlockBytes / 4];
    for (int b = 0; b < kFrameBlocks; ++b) {
        const uint32_t at = start + b * kFrameBlockBytes;
        if (!block_is_frame(rdram, at)) {
            log_once("frame: unexpected list layout, left 4:3");
            return 0;
        }
        for (uint32_t i = 0; i < kFrameBlockBytes / 4; ++i) {
            blocks[b][i] = read32(rdram, at + i * 4);
        }
    }
    // The left part (columns 0..119) is LEFT-anchored at x 0..120 and the right
    // part (columns 120..287) RIGHT-anchored at x 152..320. In the centred
    // coordinates the filler uses, the gap between them is
    // [120 - extension, 152 + extension]. The filler covers it and a pixel
    // either side; wider, it would show through the right part's cut-out
    // beside the panel.
    const int left_end = kFrameSplitColumn;
    const int right_start = 320 - (kFrameWidth - kFrameSplitColumn);
    // RT64 places a RIGHT-anchored x = 320 a little short of the wide picture's
    // last column (its misalignment correction and the expanded target's
    // rounding left 1.33 px of black at 16:9). The right part runs on past the
    // edge; its tile clamps s at column 287, so only that column repeats.
    const int right_end = 320 + kEdgeBleed;
    const int filler_left = left_end - g_frame_extension - 1;
    const int filler_right = right_start + g_frame_extension + 1;
    bool upscaled = false;
    Writer w{rdram, start};
    // The game's full-screen scissor (0x801A1B84) is (0,0)-(319,239), and its
    // lower-right edge is exclusive: it would clip the frame's last column and
    // row, which now sit on the picture's edges. The frame is drawn under an
    // extended scissor from the left edge to past the right edge (an extended
    // scissor carries no bounds, so RT64 does not clamp it to x = 320), and the
    // game's scissor comes back after it. RT64 merges every draw's scissor into
    // the frame pair's (`RDP::drawRect`), so this widens the merged scissor; the
    // map draws no projections, so nothing it draws stops qualifying for
    // Expand. Do not copy this to a screen with 3D (widescreen.md, section 2).
    w.scissor(kOriginLeft, kOriginRight, 0, 0, 320 + kEdgeBleed, 240);
    for (int b = 0; b < kFrameBlocks; ++b) {
        for (int i = 0; i < 16; i += 2) {
            w.cmd(blocks[b][i], blocks[b][i + 1]);
        }
        const uint32_t tile_size_w0 = blocks[b][12];  // SETTILESIZE 0: ult in the low 12 bits
        const uint32_t tile_size_w1 = blocks[b][13];  // lrt in the low 12 bits
        const int32_t t = static_cast<int32_t>(blocks[b][19] & 0xFFFFu);
        const int32_t uly = frame_row_y(b * kFrameRows);
        const int32_t lry = frame_row_y((b + 1) * kFrameRows);
        const int32_t dtdy = dtdy_for(kFrameRows, lry - uly);
        // A stretched strip samples at the output resolution, so the stretch
        // spreads evenly instead of doubling every tenth row.
        const bool stretched = dtdy != 0x400;
        if (stretched != upscaled) {
            w.force_upscale_2d(stretched);
            upscaled = stretched;
        }
        // Tile 1: CI 4b, line 18, TMEM word 3 (column 48), clamp t, s wraps
        // every 64 (masks 6).
        w.cmd(0xF5402400u | kFillerTmemWord, 0x01080060u);
        w.cmd(0xF2000000u | (tile_size_w0 & 0xFFFu), 0x01000000u | (252u << 12) | (tile_size_w1 & 0xFFFu));
        w.tex_rect(1, kOriginNone, kOriginNone, filler_left * 4, uly, filler_right * 4, lry, 0, t,
                   0x400, dtdy);
        w.tex_rect(0, kOriginLeft, kOriginLeft, 0, uly, left_end * 4, lry, 0, t, 0x400, dtdy);
        w.tex_rect(0, kOriginRight, kOriginRight, right_start * 4, uly, right_end * 4, lry,
                   kFrameSplitColumn * 32, t, 0x400, dtdy);
        w.pipe_sync();
    }
    if (upscaled) {
        w.force_upscale_2d(false);
    }
    w.cmd(0xED000000u, kScissorGameW1);
    set_dl_head(rdram, w.at);
    return w.at - head;
}

// 0x801A1F64 / 0x801A1F74 / 0x801A1F80 / 0x801A1FC8: the location plate is
// anchored left and moved with the frame's top-left corner (1), the
// MONTH/DATE panel anchored right and moved with the bottom-right corner (2),
// then neither (0); the fade rectangle spans both edges (3).
extern "C" void ogre_mapws_align(uint8_t* rdram, int32_t which) {
    if (!g_in_draw) {
        return;
    }
    const uint32_t head = dl_head(rdram);
    if (!in_ram(head, 32)) {
        return;
    }
    uint32_t left = kOriginNone;
    uint32_t right = kOriginNone;
    int dx = 0;
    int dy = 0;
    switch (which) {
        case 1:
            left = right = kOriginLeft;
            dx = kPlateDx;
            dy = kPlateDy;
            break;
        case 2:
            left = right = kOriginRight;
            dx = kPanelDx;
            dy = kPanelDy;
            break;
        case 3:
            left = kOriginLeft;
            right = kOriginRight;
            break;
        default:
            break;
    }
    Writer w{rdram, head};
    if (which == 3) {
        // The fade rectangle is (0,0)-(320,240). Under the game's scissor, and
        // ending at RT64's RIGHT x = 320, it would leave the frame's last
        // column and row undarkened, so its right edge and the scissor reach
        // past the picture as the frame's do.
        w.cmd(kExOp | kExRectAlign, kOriginLeft | (kOriginRight << 12));
        w.cmd(pack16(0, 0), pack16(-kRightShift + kEdgeBleed * 4, 0));
        w.scissor(kOriginLeft, kOriginRight, 0, 0, 320 + kEdgeBleed, 240);
    }
    else {
        w.rect_align(left, right, dx, dy);
    }
    set_dl_head(rdram, w.at);
    const bool corner = which == 1 || which == 2;
    g_sprite_origin = corner ? left : kOriginNone;
    g_sprite_dx = corner ? dx : 0;
    g_sprite_dy = corner ? dy : 0;
}

// 0x801A2120, the end of the map's draw: later lists (menus over the map) get
// the default anchoring back.
extern "C" void ogre_mapws_end(uint8_t* rdram) {
    if (!g_in_draw) {
        return;
    }
    g_in_draw = false;
    g_sprite_origin = kOriginNone;
    g_sprite_dx = 0;
    g_sprite_dy = 0;
    const uint32_t head = dl_head(rdram);
    if (!in_ram(head, 64)) {
        return;
    }
    Writer w{rdram, head};
    w.rect_align(kOriginNone, kOriginNone);
    w.cmd(0xED000000u, kScissorGameW1);  // still edge-anchored, as the game's own was
    w.scissor_align_reset();
    set_dl_head(rdram, w.at);
}

// RT64's presenter crops the framebuffer's last column and row, which the game
// never draws (`overscanCrop` in the RT64 patch's rt64_vi_renderer.cpp). The
// wide map draws them every frame (the frame's outer edges are opaque), so the
// crop is off while the map is up, and the frame reaches the picture's edges.
// Called on RT64's present thread.
extern "C" int ogre_overscan_crop_override() {
    const int64_t last = g_last_wide_draw_ms.load(std::memory_order_relaxed);
    return now_ms() - last < kWideDrawHoldMs ? 0 : -1;
}

// --- Hooks: sprites -----------------------------------------------------------

// func_ovlM_8019F83C(x, y, kind), before its `jr ra`. The game's version clips
// a sprite at x = 0 and y = 0 the way gSPScisTextureRectangle does, which cuts
// off every sprite in the left extension. Its block is sync, rectangle (E4, E1,
// F1), sync at `block`; while the map draw is wide the rectangle is rewritten
// in place with signed coordinates and the plate/panel anchoring and offset.
// `x_quarters` is the function's $t3, x * 4 sign-extended from 16 bits, and
// `entry` its $a2, the kind's (w, h, s, t) entry in the table at 0x801A6FD8.
extern "C" void ogre_mapws_sprite(uint8_t* rdram, uint32_t block, int32_t x_quarters, int32_t y,
                                  uint32_t entry) {
    if (!g_in_draw || !in_ram(block, 40) || !in_ram(entry, 8)) {
        return;
    }
    if ((read32(rdram, block + 8) >> 24) != 0xE4u || read32(rdram, block + 16) != 0xE1000000u ||
        read32(rdram, block + 24) != 0xF1000000u) {
        log_once("sprite: unexpected list layout, left clipped");
        return;
    }
    const int32_t w = static_cast<uint16_t>(read_s16(rdram, entry + 0));
    const int32_t h = static_cast<uint16_t>(read_s16(rdram, entry + 2));
    const int32_t s = read_s16(rdram, entry + 4);
    const int32_t t = read_s16(rdram, entry + 6);
    const int32_t sx = (static_cast<int16_t>(x_quarters) >> 2) + g_sprite_dx;
    const int32_t sy = static_cast<int16_t>(y) + g_sprite_dy;
    Writer out{rdram, block + 8};
    out.tex_rect(0, g_sprite_origin, g_sprite_origin, sx * 4, sy * 4, (sx + w) * 4, (sy + h) * 4,
                 s * 32, t * 32);
}

// func_ovlM_801A2D84 at 0x801A3298, after a location icon's rectangle is
// written at block + 8..31 (16 wide, `height` tall, texture at 0,0). Rewrites it
// with signed coordinates, in place.
extern "C" void ogre_mapws_icon(uint8_t* rdram, uint32_t block, int32_t x, int32_t y,
                                uint32_t variant, int32_t height2) {
    if (!g_in_draw || !in_ram(block, 32)) {
        return;
    }
    if ((read32(rdram, block + 8) >> 24) != 0xE4u || read32(rdram, block + 16) != 0xE1000000u ||
        read32(rdram, block + 24) != 0xF1000000u) {
        log_once("location icon: unexpected list layout, left clipped");
        return;
    }
    const int32_t sx = static_cast<int16_t>(x);
    const int32_t sy = static_cast<int16_t>(y);
    const int32_t h = (variant & 0xFFu) == 2 ? static_cast<int16_t>(height2) : 16;
    Writer w{rdram, block + 8};
    w.tex_rect(0, kOriginNone, kOriginNone, sx * 4, sy * 4, (sx + 16) * 4, (sy + h) * 4, 0, 0);
}
