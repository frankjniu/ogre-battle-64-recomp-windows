// Hang report (see hang_watch.hpp).
//
// A hang leaves a player with a frozen window and no file to attach: the crash
// handler (crash_log.cpp) only runs on a fatal signal, and the periodic `[snap]`
// queue snapshot is off by default because each one stalls its thread ~1.6 s on
// Windows (`OGRE_SNAP`, DECISIONS.md).
//
// The game's frame counter `D_800AEFA4` is the VI count `D_800C4BCC` copied at
// the end of every game frame (`func_80072398`, `sw` at `0x800726C4`). When it
// stays unchanged for `OGRE_HANG_MS` (default 5000 ms) of counted time, this
// module prints a `[hang]` line, runs the queue snapshot once, and writes
// error.log through `crash_log::write_error_log`, so the report carries the
// snapshot and the boot log from the stdout/stderr rings. It reports once per
// stall, and prints `[hang] frames resumed` if the counter moves again.
//
// Counted time excludes the boot (until the counter first changes), time while
// the game window is minimized or hidden, and any gap of more than 1 s between
// two calls (the main thread itself was blocked: a window drag, a system
// sleep). `OGRE_HANG_MS=0` turns the watch off.

#include "hang_watch.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <ultramodern/ultramodern.hpp>

#include "crash_log.hpp"

namespace ogre {
namespace {

constexpr uint32_t kFrameCounterAddress = 0x800AEFA4;
constexpr uint32_t kViCountAddress = 0x800C4BCC;
constexpr uint32_t kSceneAddress = 0x800E810E;
constexpr uint32_t kDefaultThresholdMs = 5000;
constexpr uint64_t kMaxCountedGapMs = 1000;

uint32_t threshold_ms() {
    static const uint32_t value = [] {
        const char* text = std::getenv("OGRE_HANG_MS");
        if (text == nullptr || text[0] == '\0') {
            return kDefaultThresholdMs;
        }
        return static_cast<uint32_t>(std::strtoul(text, nullptr, 10));
    }();
    return value;
}

// Host RDRAM holds each guest word in native order at `address - 0x80000000`.
uint32_t guest_u32(const uint8_t* rdram, uint32_t address) {
    return *reinterpret_cast<const uint32_t*>(rdram + (address - 0x80000000u));
}

// A halfword sits at `address ^ 2` in the host image.
uint16_t guest_u16(const uint8_t* rdram, uint32_t address) {
    return *reinterpret_cast<const uint16_t*>(rdram + ((address ^ 2u) - 0x80000000u));
}

struct HangWatch {
    bool armed = false;        // the counter has changed once since the boot
    bool reported = false;     // this stall already wrote its report
    uint32_t last_counter = 0;
    uint64_t last_tick_ms = 0;
    uint64_t last_change_ms = 0;  // wall time of the last counter change
    uint64_t still_ms = 0;        // counted time since the counter last changed
};

HangWatch g_watch;

}  // namespace

void hang_watch_tick(SDL_Window* game_window) {
    const uint32_t limit = threshold_ms();
    uint8_t* rdram = ultramodern::get_rdram_base();
    if (limit == 0 || rdram == nullptr) {
        return;
    }
    const uint64_t now = SDL_GetTicks64();
    const uint64_t gap = g_watch.last_tick_ms == 0 ? 0 : now - g_watch.last_tick_ms;
    g_watch.last_tick_ms = now;

    const uint32_t counter = guest_u32(rdram, kFrameCounterAddress);
    if (counter != g_watch.last_counter) {
        if (g_watch.reported) {
            printf("[hang] frames resumed %llu ms after the last frame (frame counter %u)\n",
                   static_cast<unsigned long long>(now - g_watch.last_change_ms), counter);
            fflush(stdout);
        }
        // The first change is the boot's first frame; the second proves the
        // frame pump is running.
        if (g_watch.last_counter != 0) {
            g_watch.armed = true;
        }
        g_watch.last_counter = counter;
        g_watch.last_change_ms = now;
        g_watch.still_ms = 0;
        g_watch.reported = false;
        return;
    }
    if (!g_watch.armed || g_watch.reported) {
        return;
    }
    const Uint32 flags = game_window != nullptr ? SDL_GetWindowFlags(game_window) : 0;
    if ((flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0 || gap > kMaxCountedGapMs) {
        return;
    }
    g_watch.still_ms += gap;
    if (g_watch.still_ms < limit) {
        return;
    }
    g_watch.reported = true;
    printf("[hang] the game produced no frame for %llu ms: frame counter D_800AEFA4=%u, "
           "VI count D_800C4BCC=%u, scene D_800E810E=0x%04X\n",
           static_cast<unsigned long long>(g_watch.still_ms), counter,
           guest_u32(rdram, kViCountAddress), guest_u16(rdram, kSceneAddress));
    fflush(stdout);
    ultramodern::debug_dump_queue_snapshot(rdram);
    fflush(stdout);
    char reason[96];
    std::snprintf(reason, sizeof(reason), "hang: the game produced no frame for %u ms", limit);
    crash_log::write_error_log(reason);
    printf("[hang] wrote error.log\n");
    fflush(stdout);
}

}  // namespace ogre
