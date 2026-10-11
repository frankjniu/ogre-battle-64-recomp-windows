// See map_cursor.hpp for the state layout and the call site.
//
// The mouse is sampled in `pump_sdl_events` (sdl_platform.cpp), which runs on
// the platform thread, and the position is applied here on the game thread.
// Plain atomics carry it across.

#include "map_cursor.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Guest memory: `recomp.h` defines MEM_W as a native u32 at `offset`,
// MEM_H as a native u16 at `offset ^ 2` and MEM_B as a native u8 at
// `offset ^ 3`, with `offset = guest - 0x80000000` for KSEG0.
constexpr uint32_t kKseg0 = 0x80000000u;
constexpr uint32_t kKseg0End = 0x80800000u;

bool in_ram(uint32_t guest, uint32_t bytes) {
    return guest >= kKseg0 && bytes <= (kKseg0End - guest);
}

uint32_t offset_of(uint32_t guest) { return guest - kKseg0; }

uint8_t read_u8(const uint8_t* rdram, uint32_t guest) {
    return rdram[offset_of(guest) ^ 3u];
}

int16_t read_s16(const uint8_t* rdram, uint32_t guest) {
    uint16_t value = 0;
    std::memcpy(&value, rdram + (offset_of(guest) ^ 2u), sizeof(value));
    return static_cast<int16_t>(value);
}

void write_s16(uint8_t* rdram, uint32_t guest, int16_t value) {
    std::memcpy(rdram + (offset_of(guest) ^ 2u), &value, sizeof(value));
}

// --- Mouse state, written by the platform thread ---------------------------

std::atomic<bool> g_mouse_live{false};
std::atomic<int> g_mouse_x{0};
std::atomic<int> g_mouse_y{0};

// The N64 D-pad bits the pointer is pushing past the map's edge, sampled with
// the pointer position. A pointer outside the 320x240 framebuffer (the
// pillarbox, or any pixel of a fullscreen window beyond the map's frame) still
// names an edge, so the direction is computed where the pointer is clamped into
// the framebuffer's range rather than where `g_mouse_live` is true.
std::atomic<uint16_t> g_edge_direction{0};

// The map module's cursor draw sets this every time it runs, and the input side
// reads it, so the `A` and the edge direction are not injected while another
// screen is up. Nothing else reports "the map is the active scene" to the
// platform thread.
std::atomic<uint32_t> g_map_draw_frame{0};
uint32_t g_game_frame = 0;

// The map state object and the RDRAM base from the last draw, so the click log
// can read the game's own cursor pixel. Both are only written on the game
// thread, and only read for the log.
std::atomic<uint32_t> g_state_pointer{0};
std::atomic<const uint8_t*> g_state_rdram{nullptr};

// True while the map's cursor draw has run within the last few frames. The
// click's `A` is armed for a handful of polls, and the draw runs once per frame,
// so the tolerance only has to cover the gap between the click and the next
// draw.
bool map_is_active() {
    const uint32_t now = g_game_frame;
    const uint32_t last = g_map_draw_frame.load(std::memory_order_acquire);
    return last != 0 && now - last <= 4u;
}

// The MOUSE ENABLED setting, published by `settings.cpp` at boot and on a
// change. Off until it says otherwise, so a run that never loads a settings file
// leaves the pointer alone.
std::atomic<bool> g_setting_enabled{false};

// How many input polls a click holds the game's `A`. The game polls the pad
// about once per emulated frame, and a real button press lasts many frames, so
// a one-poll press is at the edge of what the map's state machine samples.
// `OGRE_MAP_CURSOR_CONFIRM_POLLS` overrides it for testing.
std::atomic<int> g_confirm_polls{0};

// The state object is a heap pointer, so it is not a fixed address.
constexpr uint32_t kStatePointer = 0x80197B18u;
constexpr uint32_t kCursorDraw = 0x50u;    // u8
constexpr uint32_t kCursorX = 0x52u;       // s16
constexpr uint32_t kCursorY = 0x54u;       // s16
constexpr uint32_t kStateWritten = 0x56u;  // the draw reads no field past this

// `A` in the N64 pad report (`func_8007284C` reads `lhu 0(D_800C4BF0)`), and the
// D-pad bits.
constexpr uint16_t kButtonA = 0x8000u;
constexpr uint16_t kPadRight = 0x0100u;
constexpr uint16_t kPadLeft = 0x0200u;
constexpr uint16_t kPadDown = 0x0400u;
constexpr uint16_t kPadUp = 0x0800u;

// The play area the map state machine clamps the cursor into
// (`func_ovlM_8019AF0C` at 0x8019B8E0: x 0x28..0x110, y 0x28..0xC0). A pointer
// within `kEdgeMargin` of a bound is pushing past it.
constexpr int kPlayMinX = 0x28;
constexpr int kPlayMaxX = 0x110;
constexpr int kPlayMinY = 0x28;
constexpr int kPlayMaxY = 0xC0;
constexpr int kEdgeMarginDefault = 24;

bool enabled() {
    static const int override_state = [] {
        const char* value = std::getenv("OGRE_MAP_CURSOR");
        if (value == nullptr || value[0] == '\0') {
            return -1;
        }
        return (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0 ||
                std::strcmp(value, "false") == 0 || std::strcmp(value, "no") == 0)
                   ? 0
                   : 1;
    }();
    if (override_state >= 0) {
        return override_state != 0;
    }
    return g_setting_enabled.load(std::memory_order_relaxed);
}

bool log_enabled() {
    static const bool on = [] {
        const char* value = std::getenv("OGRE_MAP_CURSOR_LOG");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return on;
}

int confirm_polls() {
    static const int polls = [] {
        const char* value = std::getenv("OGRE_MAP_CURSOR_CONFIRM_POLLS");
        if (value == nullptr || value[0] == '\0') {
            return 8;
        }
        const int parsed = std::atoi(value);
        return parsed > 0 ? parsed : 8;
    }();
    return polls;
}

int edge_margin() {
    static const int margin = [] {
        const char* value = std::getenv("OGRE_MAP_CURSOR_EDGE");
        if (value == nullptr || value[0] == '\0') {
            return kEdgeMarginDefault;
        }
        const int parsed = std::atoi(value);
        return parsed >= 0 ? parsed : kEdgeMarginDefault;
    }();
    return margin;
}

// `OGRE_MAP_CURSOR_EDGE_FORCE=<n>` holds that raw D-pad mask (hex or decimal,
// e.g. `0x100` for right) as if the pointer were at that edge, with no mouse
// event needed. It is a test aid: it makes the edge scroll measurable in a
// scripted run that no one is driving.
uint16_t edge_forced() {
    static const uint16_t forced = []() -> uint16_t {
        const char* value = std::getenv("OGRE_MAP_CURSOR_EDGE_FORCE");
        if (value == nullptr || value[0] == '\0') {
            return 0;
        }
        return static_cast<uint16_t>(std::strtoul(value, nullptr, 0));
    }();
    return forced;
}

// The presented picture is the 320x240 framebuffer, aspect-fit and centered in
// the drawable. Mouse events are in window points; the drawable is in pixels.
// Writes the framebuffer pixel, clamped into the framebuffer's range, and
// returns false when the point is outside the framebuffer.
bool window_to_framebuffer(int window_x, int window_y, int win_w, int win_h, int draw_w,
                           int draw_h, int& fb_x, int& fb_y) {
    if (win_w <= 0 || win_h <= 0 || draw_w <= 0 || draw_h <= 0) {
        return false;
    }
    constexpr float kFbW = 320.0f;
    constexpr float kFbH = 240.0f;
    const float px = static_cast<float>(window_x) * static_cast<float>(draw_w) /
                     static_cast<float>(win_w);
    const float py = static_cast<float>(window_y) * static_cast<float>(draw_h) /
                     static_cast<float>(win_h);
    const float scale = (static_cast<float>(draw_w) / static_cast<float>(draw_h) > kFbW / kFbH)
                            ? static_cast<float>(draw_h) / kFbH
                            : static_cast<float>(draw_w) / kFbW;
    const float origin_x = (static_cast<float>(draw_w) - kFbW * scale) * 0.5f;
    const float origin_y = (static_cast<float>(draw_h) - kFbH * scale) * 0.5f;
    const float gx = (px - origin_x) / scale;
    const float gy = (py - origin_y) / scale;
    fb_x = static_cast<int>(std::min(std::max(gx, 0.0f), kFbW - 1.0f));
    fb_y = static_cast<int>(std::min(std::max(gy, 0.0f), kFbH - 1.0f));
    return gx >= 0.0f && gy >= 0.0f && gx < kFbW && gy < kFbH;
}

// The D-pad bits for a framebuffer pixel inside the map's view. A pixel left of
// the play area pushes left, and the bounds are the state machine's own clamps.
uint16_t edge_direction_for(int fb_x, int fb_y) {
    const int margin = edge_margin();
    uint16_t direction = 0;
    if (fb_x <= kPlayMinX + margin) {
        direction |= kPadLeft;
    }
    else if (fb_x >= kPlayMaxX - margin) {
        direction |= kPadRight;
    }
    if (fb_y <= kPlayMinY + margin) {
        direction |= kPadUp;
    }
    else if (fb_y >= kPlayMaxY - margin) {
        direction |= kPadDown;
    }
    return direction;
}

}  // namespace

// Called from the platform thread when the pointer moves over the game window.
extern "C" void ogre_map_cursor_note_mouse(int window_x, int window_y, int win_w, int win_h,
                                           int draw_w, int draw_h) {
    int fb_x = 0;
    int fb_y = 0;
    const bool inside =
        window_to_framebuffer(window_x, window_y, win_w, win_h, draw_w, draw_h, fb_x, fb_y);
    // The edge push uses the clamped pixel, so a pointer in the pillarbox (or
    // outside a fullscreen window's picture) still scrolls the map.
    const uint16_t direction = edge_direction_for(fb_x, fb_y);
    const uint16_t previous = g_edge_direction.exchange(direction, std::memory_order_relaxed);
    if (direction != previous && log_enabled()) {
        std::fprintf(stderr, "[mapcursor] edge push 0x%03X at (%d,%d)\n", direction, fb_x, fb_y);
        std::fflush(stderr);
    }
    if (!inside) {
        // The pointer names no framebuffer pixel, so it does not place the
        // cursor; the game's own input drives it again.
        g_mouse_live.store(false, std::memory_order_relaxed);
        return;
    }
    g_mouse_x.store(fb_x, std::memory_order_relaxed);
    g_mouse_y.store(fb_y, std::memory_order_relaxed);
    g_mouse_live.store(true, std::memory_order_release);
}

void ogre_map_cursor_clear() {
    g_mouse_live.store(false, std::memory_order_relaxed);
    g_edge_direction.store(0, std::memory_order_relaxed);
}

// Called by `settings.cpp` when the MOUSE ENABLED row is applied.
extern "C" void ogre_map_cursor_set_enabled(bool on) {
    g_setting_enabled.store(on, std::memory_order_relaxed);
}

// Called from the platform thread on a left click over the game window. The
// click itself carries no position: the pointer has already placed the cursor,
// so arming the confirm is all this does.
extern "C" void ogre_map_cursor_click(int window_x, int window_y, int win_w, int win_h,
                                      int draw_w, int draw_h) {
    if (!enabled()) {
        return;
    }
    int fb_x = 0;
    int fb_y = 0;
    if (!window_to_framebuffer(window_x, window_y, win_w, win_h, draw_w, draw_h, fb_x, fb_y)) {
        return;
    }
    if (!g_mouse_live.load(std::memory_order_relaxed)) {
        // The cursor is not following the pointer, so there is nothing to
        // confirm at (moving it first would confirm the previous position).
        return;
    }
    g_confirm_polls.store(confirm_polls(), std::memory_order_release);
    if (log_enabled()) {
        // The game's own cursor pixel is logged beside the pointer's, so a log
        // shows whether the click and the game agree on where the cursor is.
        // The map's draw last cached these; without them the click is only
        // logged with its own pixel.
        uint8_t draw = 0;
        int game_x = -1;
        int game_y = -1;
        const uint32_t pointer = g_state_pointer.load(std::memory_order_acquire);
        const uint8_t* rdram = g_state_rdram.load(std::memory_order_acquire);
        if (pointer != 0 && rdram != nullptr) {
            draw = read_u8(rdram, pointer + kCursorDraw);
            game_x = read_s16(rdram, pointer + kCursorX);
            game_y = read_s16(rdram, pointer + kCursorY);
        }
        std::fprintf(stderr,
                     "[mapcursor] click at (%d,%d) game=(%d,%d) draw=%u: A for %d poll(s)\n", fb_x,
                     fb_y, game_x, game_y, draw, confirm_polls());
        std::fflush(stderr);
    }
}

// Called from `get_input` on the game thread, once per pad poll.
extern "C" bool ogre_map_cursor_take_confirm() {
    if (!enabled()) {
        return false;
    }
    // The armed count is consumed on every poll, whatever screen is up, so a
    // click that lands on another screen expires instead of waiting to press `A`
    // on this one the next time it appears. The `A` itself is only reported
    // while the map's own cursor draw is running.
    int remaining = g_confirm_polls.load(std::memory_order_acquire);
    while (remaining > 0) {
        if (g_confirm_polls.compare_exchange_weak(remaining, remaining - 1,
                                                  std::memory_order_acq_rel)) {
            return map_is_active();
        }
    }
    return false;
}

// Called from `get_input` on the game thread, once per pad poll.
extern "C" uint16_t ogre_map_cursor_edge_direction() {
    if (!enabled() || !map_is_active()) {
        return 0;
    }
    const uint16_t forced = edge_forced();
    if (forced != 0) {
        return forced;
    }
    return g_edge_direction.load(std::memory_order_relaxed);
}

// Called from the game thread, from the map's cursor draw, just before it reads
// the cursor's position.
extern "C" void ogre_map_cursor(uint8_t* rdram, int32_t state) {
    if (rdram == nullptr || !enabled()) {
        return;
    }
    if (!in_ram(kStatePointer, 4u)) {
        return;
    }
    uint32_t pointer = 0;
    std::memcpy(&pointer, rdram + offset_of(kStatePointer), sizeof(pointer));
    if (!in_ram(pointer, kStateWritten) || pointer != static_cast<uint32_t>(state)) {
        // The caller passed the cursor state; a different pointer means the
        // map module is not the resident one.
        return;
    }

    const uint8_t draw = read_u8(rdram, pointer + kCursorDraw);
    const int16_t game_x = read_s16(rdram, pointer + kCursorX);
    const int16_t game_y = read_s16(rdram, pointer + kCursorY);
    const bool live = g_mouse_live.load(std::memory_order_acquire);
    const int mouse_x = g_mouse_x.load(std::memory_order_relaxed);
    const int mouse_y = g_mouse_y.load(std::memory_order_relaxed);

    // This is the only report that the map is the active scene, so the input
    // side can keep the click's `A` and the edge direction to this screen.
    ++g_game_frame;
    g_map_draw_frame.store(g_game_frame, std::memory_order_release);
    g_state_pointer.store(pointer, std::memory_order_relaxed);
    g_state_rdram.store(rdram, std::memory_order_relaxed);

    // Log only on change: one line per frame of pointer movement would be
    // unreadable. The draw flag is logged but never forced, so the log shows
    // the game's own value.
    if (log_enabled()) {
        static uint8_t last_draw = 0xFF;
        static int16_t last_x = -1;
        static int16_t last_y = -1;
        static int last_mouse_x = -1;
        static int last_mouse_y = -1;
        if (draw != last_draw || game_x != last_x || game_y != last_y || mouse_x != last_mouse_x ||
            mouse_y != last_mouse_y) {
            last_draw = draw;
            last_x = game_x;
            last_y = game_y;
            last_mouse_x = mouse_x;
            last_mouse_y = mouse_y;
            std::fprintf(stderr, "[mapcursor] draw=%u game=(%d,%d) mouse=%s(%d,%d)\n", draw,
                         static_cast<int>(game_x), static_cast<int>(game_y), live ? "" : "off",
                         mouse_x, mouse_y);
            std::fflush(stderr);
        }
    }

    if (!live) {
        return;
    }
    write_s16(rdram, pointer + kCursorX, static_cast<int16_t>(mouse_x));
    write_s16(rdram, pointer + kCursorY, static_cast<int16_t>(mouse_y));
}
