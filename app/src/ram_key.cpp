#include "ram_key.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "sdl_platform.hpp"

#include "ultramodern/ultramodern.hpp"

namespace ogre {
namespace {

// The runtime stores every N64 32-bit word byte-reversed, so the logical byte at
// guest offset `o` lives at physical `o ^ 3` (see AGENTS.md and `recomp.h`'s
// MEM_B). A dump that skipped this would not match the ROM.
inline uint8_t logical_byte(const uint8_t* rdram, uint32_t off) {
    return rdram[(off ^ 3u) & 0x7FFFFFu];
}

std::string config_output_dir() {
    const char* dir = std::getenv("OGRE_PREF_DIR");
    if (dir != nullptr && dir[0] != '\0') {
        return std::string(dir);
    }
    return std::string("/tmp");
}

}  // namespace

void ram_key_tick(uint8_t* rdram) {
    static const int key = [] {
        const char* v = std::getenv("OGRE_RAM_KEY");
        return (v != nullptr) ? (int)std::strtol(v, nullptr, 0) : 0;
    }();
    // OGRE_RAM_KEY_AT_MS=<n>: take one dump at that point without a key press,
    // which is what makes the path testable in a scripted run.
    static const uint32_t at_ms = [] {
        const char* v = std::getenv("OGRE_RAM_KEY_AT_MS");
        return (v != nullptr) ? (uint32_t)std::strtoul(v, nullptr, 0) : 0u;
    }();
    if ((key == 0 && at_ms == 0) || rdram == nullptr) {
        return;
    }
    static bool fired_at = false;
    bool take = false;
    if (at_ms != 0 && !fired_at && ultramodern::trace_millis() >= at_ms) {
        fired_at = true;
        take = true;
    }
    // Every press takes another dump, numbered in `counter` below: the
    // black-part investigation compares one cell before and after an animation
    // step, which is two presses in one run (session 131).
    static bool was_down = false;
    if (key != 0) {
        const bool down = console::key_down(key);
        if (down && !was_down) {
            take = true;
        }
        was_down = down;
    }
    if (!take) {
        return;
    }

    static const uint32_t base = [] {
        const char* v = std::getenv("OGRE_RAM_KEY_ADDR");
        return (v != nullptr) ? (uint32_t)std::strtoul(v, nullptr, 0) : 0x802696C8u;
    }();
    static const uint32_t len = [] {
        const char* v = std::getenv("OGRE_RAM_KEY_LEN");
        uint32_t n = (v != nullptr) ? (uint32_t)std::strtoul(v, nullptr, 0) : 0x1000u;
        if (n == 0 || n > 0x100000u) {
            n = 0x1000u;
        }
        return n;
    }();
    static const std::string dir = [] {
        const char* v = std::getenv("OGRE_RAM_KEY_OUT");
        return (v != nullptr && v[0] != '\0') ? std::string(v) : config_output_dir();
    }();
    // The slot stride for the summary. The battle part palettes sit on a 0x80
    // grid, but the grid has gaps, so the stride is a knob rather than a guess.
    static const uint32_t stride = [] {
        const char* v = std::getenv("OGRE_RAM_KEY_STRIDE");
        uint32_t n = (v != nullptr) ? (uint32_t)std::strtoul(v, nullptr, 0) : 0x80u;
        return (n == 0) ? 0x80u : n;
    }();
    // OGRE_RAM_KEY_FULL=1: write the whole 8 MiB image instead of the window, so
    // a fault that is only visible in the heap can be searched offline for the
    // pointer that names the faulty buffer. `tools/rdram.py` reads the file.
    static const bool full = [] {
        const char* v = std::getenv("OGRE_RAM_KEY_FULL");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    static int counter = 0;
    ++counter;

    if (full) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/ram-%d-full.bin", dir.c_str(), counter);
        size_t written = 0;
        if (FILE* f = std::fopen(path, "wb")) {
            for (uint32_t i = 0; i < 0x800000u; ++i) {
                const uint8_t b = logical_byte(rdram, i);
                if (std::fwrite(&b, 1, 1, f) != 1) {
                    break;
                }
                ++written;
            }
            std::fclose(f);
        }
        fprintf(stderr, "[ramkey] key %d: wrote %s (%zu bytes, whole image)\n", key, path, written);
        fflush(stderr);
        return;
    }

    const uint32_t off0 = base & 0x1FFFFFFFu;
    if (off0 + len > 0x800000u) {
        fprintf(stderr, "[ramkey] 0x%08X+0x%X is outside RDRAM\n", base, len);
        fflush(stderr);
        return;
    }

    char path[512];
    std::snprintf(path, sizeof(path), "%s/ram-%d.bin", dir.c_str(), counter);
    if (FILE* f = std::fopen(path, "wb")) {
        // Write the logical bytes, exactly as the game sees them.
        for (uint32_t i = 0; i < len; ++i) {
            const uint8_t b = logical_byte(rdram, off0 + i);
            std::fwrite(&b, 1, 1, f);
        }
        std::fclose(f);
    }

    char text[512];
    std::snprintf(text, sizeof(text), "%s/ram-%d.txt", dir.c_str(), counter);
    if (FILE* f = std::fopen(text, "w")) {
        std::fprintf(f, "# OGRE_RAM_KEY dump %d\n# guest 0x%08X, %u bytes\n", counter, base, len);
        for (uint32_t i = 0; i < len; i += 16) {
            std::fprintf(f, "%08X ", base + i);
            for (uint32_t j = 0; j < 16 && i + j < len; ++j) {
                std::fprintf(f, "%02x ", logical_byte(rdram, off0 + i + j));
            }
            std::fprintf(f, "\n");
        }
        // The slots, at OGRE_RAM_KEY_STRIDE (default 0x80, the grid the battle's
        // part palettes sit on). The stride is printed so a dump says which grid
        // it was read against.
        std::fprintf(f, "\n# slots of 0x%X bytes from 0x%08X:\n", stride, base);
        for (uint32_t slot = 0; slot * stride + stride <= len; ++slot) {
            const uint32_t at = slot * stride;
            uint32_t nonzero = 0;
            uint32_t sum = 0;
            for (uint32_t i = 0; i < stride; ++i) {
                const uint8_t b = logical_byte(rdram, off0 + at + i);
                if (b != 0) {
                    ++nonzero;
                }
                sum += b;
            }
            std::fprintf(f, "  0x%08X  nonZero %3u/%u  mean %.1f%s\n", base + at, nonzero, stride,
                         double(sum) / double(stride), (nonzero == 0) ? "   <-- ALL ZERO" : "");
        }
        std::fclose(f);
    }

    fprintf(stderr, "[ramkey] key %d: wrote %s and %s (%u bytes from 0x%08X)\n", key, path, text,
            len, base);
    fflush(stderr);
}

}  // namespace ogre
