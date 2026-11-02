// Screens with a pack backdrop: an HD image that also fills the widescreen
// sides. The title (scene 0x04) is first; the Load Game book (0x12) and the
// Hugo Report (0x08) follow at the end of the file.
//
// The title screen (scene 0x04):
//
// The title module (streamedC, ROM 0x1CE040 -> 0x80197B90, compiled into
// RecompiledFuncs) draws its backdrop in `func_8019D568` as 2D texture
// rectangles, layer by layer, each through `func_80198ECC(record, x, y)`:
//
//   state = *(u32*)0x801B80E0 (malloc(612) in the enter, func_8019B300)
//   state+0x00  sky       RGBA16 320x90  at (0, 0)
//   state+0x24  clouds    RGBA32 320x106 at (0, 24)
//   state+0x48  clouds    RGBA32 320x160 at (0, 80)
//   state+0x6C  hill      RGBA32 200x112 at (0, 126)
//   state+0x90 + 0x24*D_801BA558  the soldiers, a 3-frame CI8 animation at (22, 127)
//
// Between the layers it draws three fog overlays, `func_8019C5D4(0/2/1)`:
// white rectangles over a wrapped 64x64 I8 image, which scroll. Each fog group
// inherits the tile the layer before it left in TMEM (the renderer's OGRE_FOG
// repair depends on that), so the layers' texture loads stay in the list and
// only their rectangles are turned into no-ops.
//
// With a pack image for scene 0x04 (`ogre::hd_backgrounds_screen`):
// - at the sky's `func_80198ECC` entry, the image is staged in free RDRAM at
//   0x80600000 and drawn in strips across its whole width (signed coordinates,
//   RT64 extended GBI), with a scissor over the wide picture while Expand is on;
// - after the sky and each cloud layer, the layer's rectangles become
//   G_SPNOOPs;
// - after each fog group, while Expand is on, rectangles that continue the
//   group's tiling are appended beyond its left and right ends (not on the
//   left where the hill covers the edge).
// The hill, the soldiers, the fog, the logo and the text are the game's own
// draws. The hill stays because the soldier sprite is a black block below
// their feet that the hill, drawn after it, hides; its strips get HD
// replacements cut from the same image.
//
// Every hook runs on the game thread from the generated code
// (config/config.toml). Without a pack image every hook returns at once.
// docs/guides/hd-backgrounds.md -> "Screen backdrops" has the method.

#include "hd_backgrounds.hpp"
#include "widescreen_gbi.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#if !defined(__EMSCRIPTEN__)
#include "settings.hpp"
#endif

using namespace ogre::wsgbi;

namespace {

constexpr uint16_t kTitleScene = 0x0004;
// D_800E810E: the scene the dispatcher is running.
constexpr uint32_t kActiveScene = 0x800E810Eu;
// D_801B80E0: the title's state block.
constexpr uint32_t kStatePointer = 0x801B80E0u;
constexpr uint32_t kLayerStride = 0x24u;
// The layer records the image replaces: the sky and the two cloud layers. The
// hill (state+0x6C) stays: the soldier sprite drawn before it is a black block
// below their feet, and the hill's opaque pixels are what hide that block. Its
// strips get HD replacements cut from the same image (tools/backgrounds.py).
constexpr int kLayers = 3;
constexpr uint32_t kHillLayer = 3 * kLayerStride;

// The staging buffer. OB64 uses the first 4 MiB of RDRAM; the port runs with
// the 8 MiB expansion, and nothing in the title writes above 0x80400000
// (checked on a full RDRAM image taken at the title).
constexpr uint32_t kStage = 0x80600000u;
constexpr uint32_t kStageBytes = 0x80000u;

// Fog extensions are drawn this far past the extension, so RT64's own rounding
// never leaves a gap; the scissor trims them.
constexpr int kSlack = 2;

// F3DEX2 commands.
constexpr uint32_t kTexRect = 0xE4u;
constexpr uint32_t kSpNoop = 0xE0000000u;
// gEXSetRectAspect (G_EX_SETRECTASPECT_V1). RT64 stretches a NONE-origin
// rectangle that spans the whole scissor width to the wide picture
// (`coversScissorWidth`, rt64_framebuffer_renderer.cpp) unless the rect aspect
// is ADJUST; the backdrop strips span it, and stretched they showed the image
// 4/3 too wide.
constexpr uint32_t kExRectAspect = 0x33u;
constexpr uint32_t kAspectAuto = 0x0u;
constexpr uint32_t kAspectAdjust = 0x2u;
// G_SETOTHERMODE_H, G_MDSFT_TEXTFILT (shift 12, 2 bits): bilinear / point.
constexpr uint32_t kTextFiltW0 = 0xE3001201u;
constexpr uint32_t kTextFiltBilerp = 0x00002000u;
constexpr uint32_t kTextFiltPoint = 0x00000000u;

struct LayerCall {
    bool active = false;
    uint32_t start = 0;  // the head after the backdrop draw: the layer's own commands
};

LayerCall g_layer;
uint32_t g_fog_start = 0;
const ogre::ScreenBackdrop* g_logged = nullptr;

bool log_enabled() {
    static const bool on = std::getenv("OGRE_BG_LOG") != nullptr;
    return on;
}

uint16_t read_u16(const uint8_t* rdram, uint32_t guest) {
    return static_cast<uint16_t>(read_s16(rdram, guest));
}

// A guest byte is a native u8 at `offset ^ 3` (`MEM_B` in recomp.h).
uint8_t read_u8(const uint8_t* rdram, uint32_t guest) {
    return rdram[(guest - kKseg0) ^ 3u];
}

// The first screen row from which the hill covers the screen's left edge:
// the hill layer's top opaque row in its column 0. The hill is drawn after the
// third fog group, so the game's own fog never shows over it; the extension
// leaves those rows alone on the left, where the image continues the hill.
// INT32_MAX when the hill does not reach x = 0.
int32_t hill_left_cover_y(const uint8_t* rdram, uint32_t state) {
    const uint32_t record = read32(rdram, state + kHillLayer);
    if (!in_ram(record, 8)) {
        return INT32_MAX;
    }
    const int32_t x = read_s16(rdram, state + kHillLayer + 4);
    const int32_t y = read_s16(rdram, state + kHillLayer + 6);
    const int width = read_u16(rdram, record + 4);
    const int height = read_u16(rdram, record + 6);
    if (x > 0 || width <= -x || height <= 0 || !in_ram(record + 8, uint32_t(width * height * 4))) {
        return INT32_MAX;
    }
    const uint32_t column = record + 8u + uint32_t(-x) * 4u;
    for (int row = 0; row < height; ++row) {
        if (read_u8(rdram, column + uint32_t(row * width) * 4u + 3u) == 0xFF) {
            return y + row;
        }
    }
    return INT32_MAX;
}

// The pack backdrop for `scene`, when it is the running scene.
const ogre::ScreenBackdrop* screen_backdrop(const uint8_t* rdram, uint16_t scene) {
    if (read_u16(rdram, kActiveScene) != scene) {
        return nullptr;
    }
    bool wide = false;
#if !defined(__EMSCRIPTEN__)
    wide = ogre::widescreen_mode() != ogre::WidescreenMode::Off;
#endif
    return ogre::hd_backgrounds_screen(scene, wide);
}

const ogre::ScreenBackdrop* title_backdrop(const uint8_t* rdram) {
    return screen_backdrop(rdram, kTitleScene);
}

void log_backdrop(const ogre::ScreenBackdrop* b, const char* screen) {
    if (log_enabled() && g_logged != b) {
        g_logged = b;
        std::fprintf(stderr, "[hdbg] %s backdrop '%s' (%s) %dx%d at x %d, %d strips of %d rows\n",
                     screen, b->name, b->id, b->width, b->height, b->x,
                     (b->height + b->strip_rows - 1) / b->strip_rows, b->strip_rows);
    }
}

// Copy the image into the staging buffer in the runtime's byte order: a guest
// halfword at offset `o` is a native u16 at `o ^ 2` (`MEM_H` in recomp.h).
void stage(uint8_t* rdram, const ogre::ScreenBackdrop& b) {
    const size_t count = size_t(b.width) * size_t(b.height);
    uint8_t* base = rdram + (kStage - kKseg0);
    for (size_t i = 0; i < count; ++i) {
        const uint16_t v = b.pixels[i];
        std::memcpy(base + ((i * 2) ^ 2u), &v, sizeof(v));
    }
}

void tex_rect(Writer& w, uint32_t tile, int32_t ulx, int32_t uly, int32_t lrx, int32_t lry,
              int32_t s, int32_t t, int32_t dsdx, int32_t dtdy) {
    w.cmd(kExOp | kExTexRect, (tile & 7u) | (kOriginNone << 3) | (kOriginNone << 15));
    w.cmd(pack16(ulx, uly), pack16(lrx, lry));
    w.cmd(pack16(s, t), pack16(dsdx, dtdy));
}

// The backdrop, in strips of `strip_rows` rows from the staging buffer, at the
// layer state the sky would have drawn with (1-cycle, RGB and alpha from the
// texel). Bilinear for these strips; the layers' point filter is put back after.
uint32_t draw_backdrop(uint8_t* rdram, uint32_t head, const ogre::ScreenBackdrop& b, int ext) {
    stage(rdram, b);
    Writer w{rdram, head};
    w.enable();
    w.pipe_sync();
    if (ext > 0) {
        w.scissor_full();
    }
    w.cmd(kTextFiltW0, kTextFiltBilerp);
    w.cmd(kExOp | kExRectAspect, kAspectAdjust);
    const uint32_t line = uint32_t(b.width) * 2u / 8u;  // 64-bit TMEM words per row
    const uint32_t lrs = uint32_t(b.width - 1) * 4u;
    for (int top = 0; top < b.height; top += b.strip_rows) {
        const int rows = std::min(b.strip_rows, b.height - top);
        const uint32_t ult = uint32_t(top) * 4u;
        const uint32_t lrt = uint32_t(top + rows - 1) * 4u;
        w.cmd(0xFD100000u | uint32_t(b.width - 1), kStage);   // SETTIMG RGBA16
        w.cmd(0xF5100000u | (line << 9), 0x07000000u);       // SETTILE 7
        w.cmd(0xE6000000u, 0);                               // LOADSYNC
        w.cmd(0xF4000000u | ult, 0x07000000u | (lrs << 12) | lrt);  // LOADTILE 7
        w.pipe_sync();
        w.cmd(0xF5100000u | (line << 9), 0x00000000u);       // SETTILE 0
        w.cmd(0xF2000000u | ult, (lrs << 12) | lrt);         // SETTILESIZE 0
        w.pipe_sync();
        tex_rect(w, 0, b.x * 4, top * 4, (b.x + b.width) * 4, (top + rows) * 4, 0, top << 5,
                 0x0400, 0x0400);
    }
    w.pipe_sync();
    w.cmd(kExOp | kExRectAspect, kAspectAuto);
    w.cmd(kTextFiltW0, kTextFiltPoint);
    return w.at;
}

// Turn every G_TEXRECT in [start, end) into three G_SPNOOPs. The commands are
// all 8 bytes (F3DEX2's only 16-byte command, G_BRANCH_Z, is not used here).
int nop_rects(uint8_t* rdram, uint32_t start, uint32_t end) {
    int count = 0;
    for (uint32_t at = start; at + 8 <= end;) {
        if ((read32(rdram, at) >> 24) == kTexRect && at + 24 <= end) {
            for (uint32_t i = 0; i < 3; ++i) {
                write32(rdram, at + i * 8, kSpNoop);
                write32(rdram, at + i * 8 + 4, 0);
            }
            ++count;
            at += 24;
            continue;
        }
        at += 8;
    }
    return count;
}

struct Rect {
    int32_t ulx, uly, lrx, lry;  // 10.2
    uint32_t tile;
    int32_t s, t;                // 10.5
    int32_t dsdx, dtdy;          // 5.10
};

std::vector<Rect> read_rects(const uint8_t* rdram, uint32_t start, uint32_t end) {
    std::vector<Rect> out;
    for (uint32_t at = start; at + 8 <= end;) {
        const uint32_t w0 = read32(rdram, at);
        if ((w0 >> 24) == kTexRect && at + 24 <= end) {
            const uint32_t w1 = read32(rdram, at + 4);
            const uint32_t st = read32(rdram, at + 12);
            const uint32_t dd = read32(rdram, at + 20);
            Rect r;
            r.lrx = int32_t((w0 >> 12) & 0xFFFu);
            r.lry = int32_t(w0 & 0xFFFu);
            r.tile = (w1 >> 24) & 7u;
            r.ulx = int32_t((w1 >> 12) & 0xFFFu);
            r.uly = int32_t(w1 & 0xFFFu);
            r.s = int16_t(st >> 16);
            r.t = int16_t(st & 0xFFFFu);
            r.dsdx = int16_t(dd >> 16);
            r.dtdy = int16_t(dd & 0xFFFFu);
            out.push_back(r);
            at += 24;
            continue;
        }
        at += 8;
    }
    return out;
}

// Extend a fog group to the screen edges. The game draws each row of a group
// as a run of chunks P wide that restart at the same s (the first chunk cut at
// x = 0), over a 64x64 image that wraps. The extension continues that run:
// chunks of width P at the same s, left of the first chunk's virtual start and
// right of the last chunk. A row whose first chunk does not fit that pattern
// is left alone, and so is the left side of a row at or below `left_cover_y`.
uint32_t extend_fog(uint8_t* rdram, uint32_t start, uint32_t end, int ext, int32_t left_cover_y,
                    int* added) {
    const std::vector<Rect> rects = read_rects(rdram, start, end);
    std::map<std::pair<int32_t, int32_t>, std::vector<Rect>> rows;
    for (const Rect& r : rects) {
        rows[{r.uly, r.lry}].push_back(r);
    }
    Writer w{rdram, end};
    const int32_t left_edge = -(ext + kSlack) * 4;
    const int32_t right_edge = (320 + ext + kSlack) * 4;
    for (auto& [key, row] : rows) {
        if (row.size() < 2) {
            continue;
        }
        std::sort(row.begin(), row.end(),
                  [](const Rect& a, const Rect& b) { return a.ulx < b.ulx; });
        const Rect& first = row[0];
        const Rect& second = row[1];
        const int32_t period = second.lrx - second.ulx;
        if (period <= 0 || first.dsdx != second.dsdx || first.tile != second.tile) {
            continue;
        }
        // The first chunk's virtual start, and the s it would have at x = first.ulx.
        const int32_t chunk_start = second.ulx - period;
        const int32_t expected_s =
            second.s + ((first.ulx - chunk_start) / 4) * second.dsdx / 32;
        if (expected_s != first.s) {
            continue;
        }
        auto emit = [&](int32_t x0, int32_t x1) {
            tex_rect(w, second.tile, x0, key.first, x1, key.second, second.s, second.t,
                     second.dsdx, second.dtdy);
            ++*added;
        };
        if (first.ulx > left_edge && key.first / 4 < left_cover_y) {
            if (chunk_start < first.ulx) {
                emit(chunk_start, first.ulx);
            }
            for (int32_t x = chunk_start; x > left_edge; x -= period) {
                emit(x - period, x);
            }
        }
        const Rect& last = row.back();
        if (last.lrx < right_edge && last.lrx - last.ulx == period && last.s == second.s) {
            for (int32_t x = last.lrx; x < right_edge; x += period) {
                emit(x, x + period);
            }
        }
    }
    return w.at;
}

}  // namespace

// func_80198ECC entry: a0 is the image record about to be drawn. For the sky,
// draw the backdrop first; for any replaced layer, remember where its own
// commands start.
extern "C" void ogre_title_layer_begin(uint8_t* rdram, uint32_t record) {
    g_layer.active = false;
    const ogre::ScreenBackdrop* b = title_backdrop(rdram);
    if (b == nullptr) {
        return;
    }
    const uint32_t state = read32(rdram, kStatePointer);
    if (!in_ram(state, kLayers * kLayerStride)) {
        return;
    }
    int layer = -1;
    for (int i = 0; i < kLayers; ++i) {
        if (read32(rdram, state + uint32_t(i) * kLayerStride) == record) {
            layer = i;
            break;
        }
    }
    if (layer < 0) {
        return;
    }
    uint32_t head = dl_head(rdram);
    const size_t staged = size_t(b->width) * size_t(b->height) * 2u;
    if (!in_ram(head, 0x4000) || staged > kStageBytes) {
        return;
    }
    if (layer == 0) {
        head = draw_backdrop(rdram, head, *b, ogre::widescreen_2d_extension());
        set_dl_head(rdram, head);
        log_backdrop(b, "title");
    }
    g_layer.active = true;
    g_layer.start = head;
}

// func_80198ECC before `jr ra`: the layer's commands are [start, head).
extern "C" void ogre_title_layer_end(uint8_t* rdram) {
    if (!g_layer.active) {
        return;
    }
    g_layer.active = false;
    nop_rects(rdram, g_layer.start, dl_head(rdram));
}

// func_8019D568, before each `jal func_8019C5D4` (a fog group): the group's
// commands start at the head.
extern "C" void ogre_title_fog_begin(uint8_t* rdram) {
    g_fog_start = 0;
    if (ogre::widescreen_2d_extension() <= 0 || title_backdrop(rdram) == nullptr) {
        return;
    }
    g_fog_start = dl_head(rdram);
}

// func_8019D568, after the call returns: extend the group to the edges.
extern "C" void ogre_title_fog_end(uint8_t* rdram) {
    const uint32_t start = g_fog_start;
    g_fog_start = 0;
    const int ext = ogre::widescreen_2d_extension();
    if (start == 0 || ext <= 0) {
        return;
    }
    const uint32_t end = dl_head(rdram);
    if (!in_ram(start, 8) || end <= start || !in_ram(end, 0x4000)) {
        return;
    }
    const uint32_t state = read32(rdram, kStatePointer);
    const int32_t cover = in_ram(state, kHillLayer + kLayerStride) ? hill_left_cover_y(rdram, state)
                                                                   : INT32_MAX;
    int added = 0;
    set_dl_head(rdram, extend_fog(rdram, start, end, ext, cover, &added));
    if (log_enabled()) {
        static int logged = 0;
        if (logged < 3) {
            ++logged;
            std::fprintf(stderr, "[hdbg] title fog group 0x%08X..0x%08X: %d edge rectangle(s)\n",
                         start, end, added);
        }
    }
}

// --- The Load Game book (scene 0x12) ----------------------------------------
//
// `func_801A9C68` (the scene's per-frame hook, title module) draws a 3D scene:
// the viewport `0x801B8D10`, the projection (`func_801A8FB4`), the 3D setup
// list `0x801B8CA0`, then the object list `func_801A6BE8`, where each object is
// a G_MTX push, its texture loads, a `G_DL 0x0E000000` into its own vertex list
// and a G_POPMTX. Object 0 is the desk (a 15-vertex frame around the book, a
// CI8 32x64 wood tile that wraps along s). Objects 1 and 2 are the book's soft
// shadows (an RGBA32 16x32 image), which reach past the 4:3 edges; the book's
// leather cover spans the 4:3 width. The pages and the text follow
// (`func_801A892C` and the jump table after it).
//
// With a pack image for scene 0x12 the backdrop is drawn as 2D strips before
// the 3D setup list, and the desk's `G_DL` call becomes a G_SPNOOP. The
// shadows, the book and the text are the game's; RT64's Expand keeps the 3D
// scene centred at its own size, and the shadows fall on the backdrop.

namespace {

constexpr uint16_t kLoadGameScene = 0x0012;
constexpr uint32_t kGDl = 0xDEu;
constexpr uint32_t kSetTImg = 0xFDu;
constexpr uint32_t kObjectList = 0x0E000000u;  // segment 14, where every object's vertices live

uint32_t g_objects_start = 0;

// G_SETTIMG's format, size and width fields.
uint32_t timg_signature(uint32_t w0) {
    return w0 & 0x00FF0FFFu;
}
constexpr uint32_t kDeskTimg = (2u << 21) | (1u << 19) | (32u - 1u);    // CI8, 32 wide

}  // namespace

// Before `G_DL 0x801B8CA0` is written (0x801A9CF8): the backdrop, with the 2D
// state its strips need (1-cycle, no TLUT, opaque without Z, the texel's
// colour). The 3D setup list that follows sets the objects' own state.
extern "C" void ogre_loadgame_backdrop(uint8_t* rdram) {
    const ogre::ScreenBackdrop* b = screen_backdrop(rdram, kLoadGameScene);
    if (b == nullptr) {
        return;
    }
    const uint32_t head = dl_head(rdram);
    if (!in_ram(head, 0x4000) || size_t(b->width) * size_t(b->height) * 2u > kStageBytes) {
        return;
    }
    Writer w{rdram, head};
    w.pipe_sync();
    w.cmd(0xE3000A01u, 0x00000000u);  // G_SETOTHERMODE_H cycle type: 1-cycle
    w.cmd(0xE3001001u, 0x00000000u);  // G_SETOTHERMODE_H TLUT: none
    w.cmd(0xE3000C00u, 0x00000000u);  // G_SETOTHERMODE_H perspective correction: off
    w.cmd(0xE200001Cu, 0x0F0A4000u);  // G_SETOTHERMODE_L G_RM_OPA_SURF, G_RM_OPA_SURF2
    w.cmd(0xFCFFFFFFu, 0xFFFCF279u);  // G_SETCOMBINE: texel colour and alpha
    set_dl_head(rdram, draw_backdrop(rdram, w.at, *b, ogre::widescreen_2d_extension()));
    log_backdrop(b, "load game");
}

// Before `jal func_801A6BE8` (0x801A9D1C): the objects' commands start here.
extern "C" void ogre_loadgame_objects_begin(uint8_t* rdram) {
    g_objects_start = 0;
    if (screen_backdrop(rdram, kLoadGameScene) == nullptr) {
        return;
    }
    g_objects_start = dl_head(rdram);
}

// After the object list (0x801A9D24): the desk is not drawn.
extern "C" void ogre_loadgame_objects_end(uint8_t* rdram) {
    const uint32_t start = g_objects_start;
    g_objects_start = 0;
    const uint32_t end = dl_head(rdram);
    if (start == 0 || !in_ram(start, 8) || end <= start || !in_ram(end, 8)) {
        return;
    }
    uint32_t timg = 0;
    int object = 0;
    int dropped = 0;
    for (uint32_t at = start; at + 8 <= end; at += 8) {
        const uint32_t w0 = read32(rdram, at);
        const uint32_t op = w0 >> 24;
        if (op == kSetTImg) {
            timg = timg_signature(w0);
            continue;
        }
        if (op != kGDl || read32(rdram, at + 4) != kObjectList) {
            continue;
        }
        // Object 0 with the desk's texture: a run that starts mid-scene
        // still finds it, and any other first object is left alone.
        if (object == 0 && timg == kDeskTimg) {
            write32(rdram, at, kSpNoop);
            write32(rdram, at + 4, 0);
            ++dropped;
        }
        ++object;
    }
    if (log_enabled()) {
        static int logged = 0;
        if (logged < 2) {
            ++logged;
            std::fprintf(stderr, "[hdbg] load game objects: %d, %d not drawn\n", object, dropped);
        }
    }
}

// --- The Hugo Report (scene 0x08) -------------------------------------------
//
// The map's R menu opens the Hugo Report (scene 0x08, descriptor 0x8018FC64,
// records 14 and 30). Its backdrop is a stone tablet with a griffin relief on
// black: one CI8 320x240 image record drawn by `func_ovlK_802349AC(record)`
// (unit K, bankRec14d at 0x8022ACB0). The record is `+3` the texel size code
// (1 = 8-bit), `+4`/`+6` width and height, the texels at `+8` and the 256-entry
// RGBA16 TLUT after them. The function writes ten commands, 0x50 bytes:
//
//   G_DL 0x8023DF68     setup: 1-cycle, TLUT RGBA16, point filter, combiner
//                       TEXEL0 * PRIMITIVE + ENVIRONMENT
//   SETPRIMCOLOR white, the TLUT load (SETTIMG, TILESYNC, SETTILE 7, LOADSYNC,
//   LOADTLUT 255), PIPESYNC, TEXTURE on, PIPESYNC
//
// and then calls `func_ovlK_8022B9A0(record, 0, 0)`, which draws the image as 40
// strips of 6 rows. With a pack image for scene 0x08 the strips are replaced by
// the pack backdrop's strips (`draw_backdrop`) with the TLUT off; the setup and
// the TLUT load stay. The windows, the portrait and the text are drawn after it
// with their own state.

namespace {

constexpr uint16_t kHugoScene = 0x0008;
constexpr uint32_t kHugoSetupDl = 0x8023DF68u;
constexpr uint32_t kHugoPrologue = 0x50u;
constexpr uint32_t kGTexture = 0xD7u;
// G_SETOTHERMODE_H G_MDSFT_TEXTLUT (shift 14, 2 bits): none / RGBA16.
constexpr uint32_t kTextLutW0 = 0xE3001001u;
constexpr uint32_t kTextLutNone = 0x00000000u;
constexpr uint32_t kTextLutRgba16 = 0x00008000u;
// The griffin image: CI8 320x240, and the FNV-1a hash of its TLUT's 512 bytes
// in guest order (read from an RDRAM image of the screen, 2026-11-01). Another
// image drawn through the same function is left alone.
constexpr int kHugoWidth = 320;
constexpr int kHugoHeight = 240;
constexpr uint32_t kHugoTlutHash = 0x1E1F0BCEu;

uint32_t g_hugo_start = 0;
uint32_t g_hugo_record = 0;

uint32_t fnv1a_words(const uint8_t* rdram, uint32_t guest, uint32_t words) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < words; ++i) {
        const uint32_t v = read32(rdram, guest + i * 4u);
        for (int shift = 24; shift >= 0; shift -= 8) {
            h ^= (v >> shift) & 0xFFu;
            h *= 16777619u;
        }
    }
    return h;
}

bool is_hugo_image(const uint8_t* rdram, uint32_t record) {
    if (!in_ram(record, 8)) {
        return false;
    }
    const uint32_t texels = uint32_t(kHugoWidth * kHugoHeight);
    if (read_u8(rdram, record + 3) != 1 || read_u16(rdram, record + 4) != kHugoWidth ||
        read_u16(rdram, record + 6) != kHugoHeight || !in_ram(record + 8, texels + 512u)) {
        return false;
    }
    return fnv1a_words(rdram, record + 8u + texels, 128) == kHugoTlutHash;
}

}  // namespace

// func_ovlK_802349AC, at its second instruction: a0 is the image record.
extern "C" void ogre_hugo_image_begin(uint8_t* rdram, uint32_t record) {
    g_hugo_start = 0;
    if (screen_backdrop(rdram, kHugoScene) == nullptr) {
        return;
    }
    g_hugo_start = dl_head(rdram);
    g_hugo_record = record;
}

// func_ovlK_802349AC, after `jal func_ovlK_8022B9A0` returns: the image's
// commands are [start, head). Keep the first 0x50 bytes and write the pack
// backdrop over the strips.
extern "C" void ogre_hugo_image_end(uint8_t* rdram) {
    const uint32_t start = g_hugo_start;
    g_hugo_start = 0;
    if (start == 0) {
        return;
    }
    const ogre::ScreenBackdrop* b = screen_backdrop(rdram, kHugoScene);
    const uint32_t end = dl_head(rdram);
    if (b == nullptr || !in_ram(start, kHugoPrologue) || end < start + kHugoPrologue ||
        !in_ram(end, 0x4000) ||
        size_t(b->width) * size_t(b->height) * 2u > kStageBytes) {
        return;
    }
    const bool layout = read32(rdram, start) == 0xDE000000u &&
                        read32(rdram, start + 4) == kHugoSetupDl &&
                        (read32(rdram, start + 64) >> 24) == kGTexture;
    if (!layout || !is_hugo_image(rdram, g_hugo_record)) {
        if (log_enabled()) {
            static int logged = 0;
            if (logged < 3) {
                ++logged;
                std::fprintf(stderr, "[hdbg] hugo report: image 0x%08X left alone (%s)\n",
                             g_hugo_record, layout ? "not the griffin image" : "unknown layout");
            }
        }
        return;
    }
    Writer w{rdram, start + kHugoPrologue};
    w.pipe_sync();
    w.cmd(kTextLutW0, kTextLutNone);
    Writer after{rdram, draw_backdrop(rdram, w.at, *b, ogre::widescreen_2d_extension())};
    after.pipe_sync();
    after.cmd(kTextLutW0, kTextLutRgba16);
    set_dl_head(rdram, after.at);
    log_backdrop(b, "hugo report");
}
