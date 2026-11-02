// The extension shared by the patched 2D widescreen screens. See
// widescreen_gbi.hpp.

#include "widescreen_gbi.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace {

// The world map's scroll range must stay non-empty: 256 - 2 * 120 = 16. The
// other screens draw their extensions up to the same width.
constexpr int kMaxExtension = 120;

std::atomic<int> g_extension{0};

}  // namespace

namespace ogre {

int widescreen_2d_extension() {
    return g_extension.load(std::memory_order_relaxed);
}

void widescreen_2d_set_extension(int pixels) {
    pixels = std::clamp(pixels, 0, kMaxExtension);
    const int previous = g_extension.exchange(pixels, std::memory_order_relaxed);
    if (previous != pixels) {
        std::fprintf(stderr, "[ws2d] extension %d px per side\n", pixels);
        std::fflush(stderr);
    }
}

}  // namespace ogre
