// See mission_cursor.hpp for the state layout and the call site.
//
// The mouse is sampled in `pump_sdl_events` (sdl_platform.cpp), which runs on
// the platform thread, and the position is applied here on the game thread.
// Plain atomics carry it across. This mirrors `app/src/map_cursor.cpp`; the two
// screens keep their own play areas, their own knobs and their own frame
// counter, because "which screen is up" is what gates the click's `A`.

#include "mission_cursor.hpp"

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

uint32_t read_u32(const uint8_t* rdram, uint32_t guest) {
    uint32_t value = 0;
    std::memcpy(&value, rdram + offset_of(guest), sizeof(value));
    return value;
}

uint8_t read_u8(const uint8_t* rdram, uint32_t guest) {
    return rdram[offset_of(guest) ^ 3u];
}

uint16_t read_u16(const uint8_t* rdram, uint32_t guest) {
    uint16_t value = 0;
    std::memcpy(&value, rdram + (offset_of(guest) ^ 2u), sizeof(value));
    return value;
}

float read_f32(const uint8_t* rdram, uint32_t guest) {
    const uint32_t bits = read_u32(rdram, guest);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void write_f32(uint8_t* rdram, uint32_t guest, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    std::memcpy(rdram + offset_of(guest), &bits, sizeof(bits));
}

// --- Mouse state, written by the platform thread ---------------------------

std::atomic<bool> g_mouse_live{false};
// Framebuffer pixels. Floats, not ints, because the hook adopts the game's own
// cursor position when the game moves it itself, and the game's pull towards the
// screen centre is asymptotic: rounding the adopted value to a whole pixel would
// discard the sub-pixel part of a slow pull.
std::atomic<float> g_mouse_x{0.0f};
std::atomic<float> g_mouse_y{0.0f};

// The N64 D-pad bits the pointer is pushing past the play area, sampled with the
// pointer position. A pointer outside the 320x240 framebuffer (the pillarbox, or
// any pixel of a fullscreen window beyond the picture) still names an edge, so
// the direction is computed where the pointer is clamped into the framebuffer's
// range rather than where `g_mouse_live` is true.
std::atomic<uint16_t> g_edge_direction{0};

// The mover runs once per frame, so its call site is the only report that the
// mission field is the active screen. The input side reads the counter, so the
// click's `A` is not injected while another screen is up.
std::atomic<uint32_t> g_mission_frame{0};
uint32_t g_game_frame = 0;

// Game-thread only: whether the mover integrated a cursor movement on this call.
// `ogre_mission_cursor_note_move` sets it at 0x801BEC04, where the delta bytes
// still hold this call's values, and the placement at the epilogue consumes and
// clears it. The mover zeroes those bytes at 0x801BEC48-0x801BEC58, so by the
// epilogue the evidence is gone.
bool g_moved_by_mover = false;

// The state object and the RDRAM base from the last call, so the click log can
// read the game's own cursor pixel. Both are only written on the game thread,
// and only read for the log.
std::atomic<const uint8_t*> g_state_rdram{nullptr};

// The MOUSE ENABLED setting, published by `settings.cpp` at boot and on a
// change. Off until it says otherwise, so a run that never loads a settings file
// leaves the pointer alone.
std::atomic<bool> g_setting_enabled{false};

// True while the mover has run within the last few frames. The click's `A` is
// armed for a handful of polls and the mover runs once per frame, so the
// tolerance only has to cover the gap between the click and the next call.
bool mission_is_active() {
    const uint32_t now = g_game_frame;
    const uint32_t last = g_mission_frame.load(std::memory_order_acquire);
    return last != 0 && now - last <= 4u;
}

// The state object is a fixed address. Record 7 reaches it through its own data
// word; the hook requires both to agree before it writes anything.
constexpr uint32_t kStatePointer = 0x801AB870u;
constexpr uint32_t kState = 0x800E91D0u;
constexpr uint32_t kCursorX = 0x08u;      // f32
constexpr uint32_t kCursorY = 0x0Cu;      // f32
constexpr uint32_t kStateWritten = 0x10u;  // the mover reads no field past this

// The mover's own input: `*(u32*)0x800E8108` is a pointer slot the base ELF's
// `func_80077494` fills with `&D_800E79B0` (the held button word) for the active
// pad, or with a zeroed block when no pad is active. The mover reads it at
// 0x801BE574-0x801BE584 and compares it against the profile's button masks.
constexpr uint32_t kHeldPointer = 0x800E8108u;
constexpr uint16_t kPadDirections = 0x0F00u;  // right 0x100, left 0x200, down 0x400, up 0x800

// The input profile the mover masks the held word against: `*(u32*)0x801F0CC0`,
// a per-mode table of button masks (`0x801E7E04` indexed by `*(0x801F0E04)`).
// Entry +0x1C is 0x0002, C-LEFT, and it gates the mover's screen-centring branch
// at 0x801BE890. `*(0x801F0E00)` is the field mode; that branch runs for modes
// below 2 and for mode 42.
constexpr uint32_t kProfilePointer = 0x801F0CC0u;
constexpr uint32_t kProfileRight = 0x0Eu;
constexpr uint32_t kProfileLeft = 0x0Cu;
constexpr uint32_t kProfileUp = 0x08u;
constexpr uint32_t kProfileDown = 0x0Au;
constexpr uint32_t kProfileCentreButton = 0x1Cu;
constexpr uint32_t kFieldMode = 0x801F0E00u;

// The mover's per-frame cursor delta bytes. They are non-zero exactly when the
// mover integrated a movement into the pair this call, from either the held
// D-pad or its own scripted nudge (`*(0x80196B0E)`, 0x801BE558-0x801BE568):
// `x += delta_x / 128 * 32`, `y -= delta_y / 128 * 32` at 0x801BE924-0x801BE988.
// The mover zeroes them once neither source applies (0x801BEC48-0x801BEC58).
constexpr uint32_t kDeltaX = 0x06u;  // s8
constexpr uint32_t kDeltaY = 0x07u;  // s8

// `A` in the N64 pad report (`func_8007284C` reads `lhu 0(D_800C4BF0)`).
constexpr uint16_t kButtonA = 0x8000u;

// The D-pad bits the pointer can hold. They are the same masks the mover
// compares the held word against (`*(0x801F0CC0)`: right +0x0E, left +0x0C, up
// +0x08, down +0x0A), so a held bit reaches the mover as if a pad had sent it.
constexpr uint16_t kPadRight = 0x0100u;
constexpr uint16_t kPadLeft = 0x0200u;
constexpr uint16_t kPadDown = 0x0400u;
constexpr uint16_t kPadUp = 0x0800u;

// The camera the field renders through: `*(u32*)0x8018F5AC` is a pointer to a
// struct whose +0x14 is the eye position (x at +0x14, y at +0x18, z at +0x1C)
// and whose +0x20 is the look-at point (x at +0x20, y at +0x24, z at +0x28);
// `func_ovlA_801AAE30` builds the view basis from those two vectors and
// intersects the ground plane at y = 0 to map a screen pixel to a world point.
// `func_ovlN_801BECF0` pans by adding the same amount to the eye and the
// look-at (0x801BEE04-0x801BEE1C for x, 0x801BEE9C-0x801BEF5C for z), which is
// what a mouse edge push is measured against. The word is
// `lw $v0, -2644($v0)` with `lui $v0, 0x8019`, so the address is
// `0x80190000 - 0xA54`.
constexpr uint32_t kCameraPointer = 0x8018F5ACu;
constexpr uint32_t kCameraEyeX = 0x14u;
constexpr uint32_t kCameraEyeZ = 0x1Cu;
constexpr uint32_t kCameraLookX = 0x20u;
constexpr uint32_t kCameraLookZ = 0x28u;

// The play area the mover clamps the cursor into (x at 0x801BEB98/0x801BEBB8,
// y at 0x801BEBE0/0x801BEC00). A pointer outside it would be clamped by the
// game anyway; clamping here keeps the logged value equal to the stored one.
constexpr float kPlayMinX = 40.0f;
constexpr float kPlayMaxX = 280.0f;
constexpr float kPlayMinY = 30.0f;
constexpr float kPlayMaxY = 210.0f;
constexpr int kEdgeMarginDefault = 24;


// Whether the port drives this screen's cursor with the pointer. The MOUSE
// ENABLED setting (app/src/settings.cpp) is the player-facing switch and is off
// by default; `OGRE_MISSION_CURSOR` overrides it for one run: `1`/`on` forces it
// on, `0`/`off` forces it off, and unset follows the setting.
bool enabled() {
    static const int override_state = [] {
        const char* value = std::getenv("OGRE_MISSION_CURSOR");
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
        const char* value = std::getenv("OGRE_MISSION_CURSOR_LOG");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return on;
}

int confirm_polls() {
    static const int polls = [] {
        const char* value = std::getenv("OGRE_MISSION_CURSOR_CONFIRM_POLLS");
        if (value == nullptr || value[0] == '\0') {
            return 8;
        }
        const int parsed = std::atoi(value);
        return parsed > 0 ? parsed : 8;
    }();
    return polls;
}

// After a click the game does something of its own: a command recentres the
// camera, and the cursor is moved with it (measured: the mover or another writer
// moves the pair on nearly every frame, 1-9 px, whether or not a button is held).
// The pointer therefore yields for this many frames after a click, adopting the
// game's result instead of overwriting it. That is what leaves the cursor on the
// place that was just acted on, so a second click confirms it. Without the yield
// the only two options are "the mouse always wins", which discards the recentre,
// or "the game always wins", which locks the mouse out of a cursor the game is
// moving every frame. The window only has to cover the camera coming to rest, so
// it is short: while it lasts the pointer does not respond, and the player has to
// move the mouse afterwards to re-sync it (developer-tuned, session 128).
// `OGRE_MISSION_CURSOR_YIELD` overrides it.
constexpr int kYieldFramesDefault = 15;  // ~0.25 s at 60 Hz

// How many input polls a click holds the game's `A`. A real button press lasts
// many frames, so a one-poll press is at the edge of what the field's own
// edge-triggered handlers sample. `OGRE_MISSION_CURSOR_CONFIRM_POLLS` overrides
// it for testing.
std::atomic<int> g_confirm_polls{0};

// Frames left in the post-click yield, counted down by the hook.
std::atomic<int> g_yield_frames{0};

int yield_frames() {
    static const int frames = [] {
        const char* value = std::getenv("OGRE_MISSION_CURSOR_YIELD");
        if (value == nullptr || value[0] == '\0') {
            return kYieldFramesDefault;
        }
        const int parsed = std::atoi(value);
        return parsed >= 0 ? parsed : kYieldFramesDefault;
    }();
    return frames;
}

int edge_margin() {
    static const int margin = [] {
        const char* value = std::getenv("OGRE_MISSION_CURSOR_EDGE");
        if (value == nullptr || value[0] == '\0') {
            return kEdgeMarginDefault;
        }
        const int parsed = std::atoi(value);
        return parsed >= 0 ? parsed : kEdgeMarginDefault;
    }();
    return margin;
}

// `OGRE_MISSION_CURSOR_EDGE_FORCE=<n>` holds that raw D-pad mask (hex or
// decimal, e.g. `0x100` for right) as if the pointer were at that edge, with no
// mouse event needed. It is a test aid: it makes the edge scroll measurable in a
// scripted run that no one is driving.
uint16_t edge_forced() {
    static const uint16_t forced = []() -> uint16_t {
        const char* value = std::getenv("OGRE_MISSION_CURSOR_EDGE_FORCE");
        if (value == nullptr || value[0] == '\0') {
            return 0;
        }
        return static_cast<uint16_t>(std::strtoul(value, nullptr, 0));
    }();
    return forced;
}

// The D-pad bits for a framebuffer pixel inside the field's view. A pixel within
// `kEdgeMargin` of a play-area bound pushes that way, and the bounds are the
// mover's own clamps, so the push starts where the cursor would stop moving.
uint16_t edge_direction_for(int fb_x, int fb_y) {
    const int margin = edge_margin();
    uint16_t direction = 0;
    if (fb_x <= static_cast<int>(kPlayMinX) + margin) {
        direction |= kPadLeft;
    }
    else if (fb_x >= static_cast<int>(kPlayMaxX) - margin) {
        direction |= kPadRight;
    }
    if (fb_y <= static_cast<int>(kPlayMinY) + margin) {
        direction |= kPadUp;
    }
    else if (fb_y >= static_cast<int>(kPlayMaxY) - margin) {
        direction |= kPadDown;
    }
    return direction;
}

// The camera's eye and look-at x/z, or false when the field's module is not the
// resident one. Only the log reads these.
bool read_camera(const uint8_t* rdram, float& eye_x, float& eye_z, float& look_x, float& look_z) {
    if (!in_ram(kCameraPointer, 4u)) {
        return false;
    }
    const uint32_t camera = read_u32(rdram, kCameraPointer);
    if (camera == 0 || !in_ram(camera, kCameraLookZ + 4u)) {
        return false;
    }
    eye_x = read_f32(rdram, camera + kCameraEyeX);
    eye_z = read_f32(rdram, camera + kCameraEyeZ);
    look_x = read_f32(rdram, camera + kCameraLookX);
    look_z = read_f32(rdram, camera + kCameraLookZ);
    return true;
}

// `OGRE_MISSION_CURSOR_AT=<x>,<y>` parks the pointer at that framebuffer pixel,
// with no mouse event, exactly as if it had been moved there. It is a test aid:
// it makes the placement measurable in a scripted run that no one is driving, and
// it is the only way to exercise the write path without a human at the mouse. It
// parks the pointer once, so the D-pad can take the cursor away from it and the
// handover behaves as it does with a real pointer.
bool forced_point(int& x, int& y) {
    const char* value = std::getenv("OGRE_MISSION_CURSOR_AT");
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    x = static_cast<int>(std::atof(value));
    const char* comma = std::strchr(value, ',');
    y = comma == nullptr ? 0 : static_cast<int>(std::atof(comma + 1));
    return true;
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

// One u16 mask out of the input profile the mover masks the held word against.
uint16_t profile_mask(const uint8_t* rdram, uint32_t offset) {
    if (!in_ram(kProfilePointer, 4u)) {
        return 0;
    }
    const uint32_t profile = read_u32(rdram, kProfilePointer);
    if (profile == 0 || !in_ram(profile, offset + 2u)) {
        return 0;
    }
    return read_u16(rdram, profile + offset);
}

// True for the field modes whose mover takes the screen-centring branch when the
// centre button is held (0x801BE85C-0x801BE868: `mode < 2 || mode == 42`).
bool field_centres(const uint8_t* rdram) {
    if (!in_ram(kFieldMode, 4u)) {
        return false;
    }
    const uint32_t mode = read_u32(rdram, kFieldMode);
    return mode < 2u || mode == 42u;
}

// The held button word the game's own input produced for the active pad, or 0
// when no pad is active. Reading it here is what lets the D-pad keep the cursor
// while it is held: this hook runs after the mover has already acted on it.
uint16_t held_buttons(const uint8_t* rdram) {
    if (!in_ram(kHeldPointer, 4u)) {
        return 0;
    }
    const uint32_t pointer = read_u32(rdram, kHeldPointer);
    if (pointer == 0 || !in_ram(pointer, 2u)) {
        return 0;
    }
    return read_u16(rdram, pointer);
}

}  // namespace

// Called from the platform thread when the pointer moves over the game window.
extern "C" void ogre_mission_cursor_note_mouse(int window_x, int window_y, int win_w, int win_h,
                                               int draw_w, int draw_h) {
    int fb_x = 0;
    int fb_y = 0;
    const bool inside =
        window_to_framebuffer(window_x, window_y, win_w, win_h, draw_w, draw_h, fb_x, fb_y);
    // The edge push uses the clamped pixel, so a pointer in the pillarbox (or
    // outside a fullscreen window's picture) still scrolls the field.
    const uint16_t direction = edge_direction_for(fb_x, fb_y);
    const uint16_t previous = g_edge_direction.exchange(direction, std::memory_order_relaxed);
    if (direction != previous && log_enabled()) {
        std::fprintf(stderr, "[missioncursor] edge push 0x%03X at (%d,%d)\n", direction, fb_x,
                     fb_y);
        std::fflush(stderr);
    }
    if (!inside) {
        // The pointer names no framebuffer pixel, so it does not place the
        // cursor; the game's own input drives it again.
        g_mouse_live.store(false, std::memory_order_relaxed);
        return;
    }
    g_mouse_x.store(static_cast<float>(fb_x), std::memory_order_relaxed);
    g_mouse_y.store(static_cast<float>(fb_y), std::memory_order_relaxed);
    g_mouse_live.store(true, std::memory_order_release);
}

extern "C" void ogre_mission_cursor_clear() {
    g_mouse_live.store(false, std::memory_order_relaxed);
    g_edge_direction.store(0, std::memory_order_relaxed);
}

// Called by `settings.cpp` when the MOUSE ENABLED row is applied.
extern "C" void ogre_mission_cursor_set_enabled(bool on) {
    g_setting_enabled.store(on, std::memory_order_relaxed);
}

// Called from the platform thread on a left click over the game window. The
// click itself carries no position: the pointer has already placed the cursor,
// so arming the confirm is all this does.
extern "C" void ogre_mission_cursor_click(int window_x, int window_y, int win_w, int win_h,
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
    g_yield_frames.store(yield_frames(), std::memory_order_release);
    if (log_enabled()) {
        // The game's own cursor pixel is logged beside the pointer's, so a log
        // shows whether the click and the game agree on where the cursor is.
        // The mover last cached the RDRAM base; without it the click is only
        // logged with its own pixel.
        float game_x = -1.0f;
        float game_y = -1.0f;
        const uint8_t* rdram = g_state_rdram.load(std::memory_order_acquire);
        if (rdram != nullptr) {
            game_x = read_f32(rdram, kState + kCursorX);
            game_y = read_f32(rdram, kState + kCursorY);
        }
        std::fprintf(stderr,
                     "[missioncursor] click at (%d,%d) game=(%.1f,%.1f): A for %d poll(s)\n", fb_x,
                     fb_y, static_cast<double>(game_x), static_cast<double>(game_y),
                     confirm_polls());
        std::fflush(stderr);
    }
}

// Called from `get_input` on the game thread, once per pad poll.
extern "C" bool ogre_mission_cursor_take_confirm() {
    if (!enabled()) {
        return false;
    }
    // The armed count is consumed on every poll, whatever screen is up, so a
    // click that lands on another screen expires instead of waiting to press `A`
    // on this one the next time it appears. The `A` itself is only reported
    // while the field's own cursor mover is running.
    int remaining = g_confirm_polls.load(std::memory_order_acquire);
    while (remaining > 0) {
        if (g_confirm_polls.compare_exchange_weak(remaining, remaining - 1,
                                                  std::memory_order_acq_rel)) {
            const bool active = mission_is_active();
            if (active && log_enabled()) {
                std::fprintf(stderr, "[missioncursor] confirm: A held for one poll\n");
                std::fflush(stderr);
            }
            return active;
        }
    }
    return false;
}

// Called from `get_input` on the game thread, once per pad poll.
extern "C" uint16_t ogre_mission_cursor_edge_direction() {
    if (!enabled() || !mission_is_active()) {
        return 0;
    }
    const uint16_t forced = edge_forced();
    if (forced != 0) {
        return forced;
    }
    return g_edge_direction.load(std::memory_order_relaxed);
}

// Called from the game thread at the mover's `L_801BEC04`, the one point on the
// mover's default path where its per-frame delta bytes still hold this call's
// values. Records whether the mover moved the cursor itself this call.
extern "C" void ogre_mission_cursor_note_move(uint8_t* rdram) {
    if (rdram == nullptr || !enabled()) {
        return;
    }
    if (!in_ram(kStatePointer, 4u)) {
        return;
    }
    const uint32_t pointer = read_u32(rdram, kStatePointer);
    if (pointer != kState || !in_ram(pointer, kStateWritten)) {
        return;
    }
    g_moved_by_mover =
        static_cast<int8_t>(read_u8(rdram, pointer + kDeltaX)) != 0 ||
        static_cast<int8_t>(read_u8(rdram, pointer + kDeltaY)) != 0;
}

// Called from the game thread, from the mission field's cursor mover, at its
// epilogue, so it runs on both of the mover's paths.
extern "C" void ogre_mission_cursor(uint8_t* rdram) {
    if (rdram == nullptr || !enabled()) {
        return;
    }
    if (!in_ram(kStatePointer, 4u)) {
        return;
    }
    const uint32_t pointer = read_u32(rdram, kStatePointer);
    if (pointer != kState || !in_ram(pointer, kStateWritten)) {
        // The module's own data word names the field's cursor state; a different
        // pointer means the mission's field module is not the resident one.
        return;
    }

    // One line per process, so a run's log says the hook is in this binary and
    // that the mission's field module is the one that reached it. The four
    // direction masks the mover compares the held word against are printed with
    // it: the edge push holds the N64 D-pad bits, so this line says whether this
    // mode's own profile uses those same bits.
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::fprintf(stderr,
                     "[missioncursor] hook active on the mission field (profile right=0x%04X "
                     "left=0x%04X up=0x%04X down=0x%04X)\n",
                     static_cast<unsigned>(profile_mask(rdram, kProfileRight)),
                     static_cast<unsigned>(profile_mask(rdram, kProfileLeft)),
                     static_cast<unsigned>(profile_mask(rdram, kProfileUp)),
                     static_cast<unsigned>(profile_mask(rdram, kProfileDown)));
        std::fflush(stderr);
    }

    // This is the only report that the field is the active screen, so the input
    // side can keep the click's `A` to this screen.
    ++g_game_frame;
    g_mission_frame.store(g_game_frame, std::memory_order_release);
    g_state_rdram.store(rdram, std::memory_order_relaxed);

    // A parked pointer (`OGRE_MISSION_CURSOR_AT`) is seeded once, on the first
    // call, and behaves like a real one from then on.
    static bool seeded = false;
    if (!seeded) {
        seeded = true;
        int park_x = 0;
        int park_y = 0;
        if (forced_point(park_x, park_y)) {
            g_mouse_x.store(static_cast<float>(park_x), std::memory_order_relaxed);
            g_mouse_y.store(static_cast<float>(park_y), std::memory_order_relaxed);
            g_mouse_live.store(true, std::memory_order_release);
        }
    }

    const bool live = g_mouse_live.load(std::memory_order_acquire);
    float mouse_x = g_mouse_x.load(std::memory_order_relaxed);
    float mouse_y = g_mouse_y.load(std::memory_order_relaxed);

    const float game_x = read_f32(rdram, pointer + kCursorX);
    const float game_y = read_f32(rdram, pointer + kCursorY);

    // The two ways the mover moves the cursor by itself this frame, read from the
    // same words the mover read:
    //
    //  * a direction is held, so the mover integrated its delta into the pair;
    //  * C-LEFT is held in a mode that centres (0x801BE830-0x801BE868), so the
    //    mover pulled the pair a third of the way to the screen centre
    //    (0x801BE890-0x801BE8E4). That branch jumps to the epilogue, which is why
    //    this hook is inserted there.
    //
    // Both are the game's own cursor motion and neither is the pointer's, so the
    // pointer adopts the result instead of overwriting it. Overwriting the
    // centring pull is what left the cursor off the place the player had just
    // acted on while the camera moved to centre it.
    const uint16_t held = held_buttons(rdram);
    const bool pad_driving = (held & kPadDirections) != 0;
    const bool centring =
        (held & profile_mask(rdram, kProfileCentreButton)) != 0 && field_centres(rdram);
    const bool integrating = g_moved_by_mover;
    g_moved_by_mover = false;
    // A click's yield window, counted down one frame per call.
    const bool yielding = g_yield_frames.load(std::memory_order_acquire) > 0;
    if (yielding) {
        g_yield_frames.fetch_sub(1, std::memory_order_acq_rel);
    }
    const bool game_driving = pad_driving || centring || integrating || yielding;
    if (game_driving) {
        g_mouse_x.store(game_x, std::memory_order_relaxed);
        g_mouse_y.store(game_y, std::memory_order_relaxed);
        mouse_x = game_x;
        mouse_y = game_y;
    }

    const float x = std::min(std::max(mouse_x, kPlayMinX), kPlayMaxX);
    const float y = std::min(std::max(mouse_y, kPlayMinY), kPlayMaxY);

    // The camera's eye and look-at x/z, so a log shows whether the field's view
    // actually panned. `func_ovlN_801BECF0` moves both together.
    float cam_eye_x = 0.0f;
    float cam_eye_z = 0.0f;
    float cam_look_x = 0.0f;
    float cam_look_z = 0.0f;
    const bool cam_ok = read_camera(rdram, cam_eye_x, cam_eye_z, cam_look_x, cam_look_z);

    // Log only on change: one line per frame of pointer movement would be
    // unreadable.
    if (log_enabled()) {
        static float last_game_x = -1.0f;
        static float last_game_y = -1.0f;
        static float last_mouse_x = -1.0f;
        static float last_mouse_y = -1.0f;
        static float last_cam_eye_x = 1.0e30f;
        static float last_cam_eye_z = 1.0e30f;
        static float last_cam_look_x = 1.0e30f;
        static float last_cam_look_z = 1.0e30f;
        static bool last_live = false;
        static bool last_pad = false;
        static bool last_centre = false;
        static bool last_integrate = false;
        static bool last_yield = false;
        const bool cam_changed =
            cam_ok && (cam_eye_x != last_cam_eye_x || cam_eye_z != last_cam_eye_z ||
                       cam_look_x != last_cam_look_x || cam_look_z != last_cam_look_z);
        if (game_x != last_game_x || game_y != last_game_y || mouse_x != last_mouse_x ||
            mouse_y != last_mouse_y || live != last_live || pad_driving != last_pad ||
            centring != last_centre || integrating != last_integrate ||
            yielding != last_yield || cam_changed) {
            last_game_x = game_x;
            last_game_y = game_y;
            last_mouse_x = mouse_x;
            last_mouse_y = mouse_y;
            last_live = live;
            last_pad = pad_driving;
            last_centre = centring;
            last_integrate = integrating;
            last_yield = yielding;
            last_cam_eye_x = cam_eye_x;
            last_cam_eye_z = cam_eye_z;
            last_cam_look_x = cam_look_x;
            last_cam_look_z = cam_look_z;
            std::fprintf(stderr,
                         "[missioncursor] game=(%.2f,%.2f) mouse=%s(%.2f,%.2f) "
                         "pad=%d centre=%d nudge=%d yield=%d held=0x%04X mode=%u "
                         "edge=0x%03X cam=%.2f,%.2f/%.2f,%.2f -> (%.2f,%.2f)\n",
                         static_cast<double>(game_x), static_cast<double>(game_y),
                         live ? "" : "off",
                         static_cast<double>(mouse_x), static_cast<double>(mouse_y),
                         pad_driving ? 1 : 0, centring ? 1 : 0, integrating ? 1 : 0,
                         yielding ? 1 : 0, static_cast<unsigned>(held),
                         static_cast<unsigned>(read_u32(rdram, kFieldMode)),
                         static_cast<unsigned>(g_edge_direction.load(std::memory_order_relaxed)),
                         static_cast<double>(cam_eye_x), static_cast<double>(cam_eye_z),
                         static_cast<double>(cam_look_x), static_cast<double>(cam_look_z),
                         static_cast<double>(x), static_cast<double>(y));
            std::fflush(stderr);
        }
    }

    if (game_driving || !live) {
        return;
    }
    write_f32(rdram, pointer + kCursorX, x);
    write_f32(rdram, pointer + kCursorY, y);
}
