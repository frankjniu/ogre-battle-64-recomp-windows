// Cheats from the CHEATS tab (see cheats.hpp).

#include "cheats.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <ultramodern/ultramodern.hpp>

#include "settings.hpp"

namespace ogre {
namespace {

constexpr uint32_t kGothAddress = 0x80196A8Cu;
constexpr uint32_t kGothMax = 9999999u;

struct ItemList {
    uint32_t address;
    int slots;
};

constexpr ItemList kItemLists[] = {
    {0x80196B20u, 278},
    {0x80193AE0u, 40},
};
constexpr uint32_t kItemSlotSize = 4;
constexpr uint32_t kItemCountOffset = 3;
constexpr uint8_t kItemMax = 99;

// The runtime keeps RDRAM in host order: a guest word is a native u32 at
// `address - 0x80000000`, a halfword a native u16 at that offset `^ 2`, a byte
// at `^ 3` (recomp.h `MEM_W`/`MEM_H`/`MEM_B`).
inline uint32_t offset_of(uint32_t address) {
    return address & 0x1FFFFFFFu;
}

uint32_t load_u32(const uint8_t* rdram, uint32_t address) {
    uint32_t value = 0;
    std::memcpy(&value, rdram + offset_of(address), sizeof(value));
    return value;
}

void store_u32(uint8_t* rdram, uint32_t address, uint32_t value) {
    std::memcpy(rdram + offset_of(address), &value, sizeof(value));
}

uint16_t load_u16(const uint8_t* rdram, uint32_t address) {
    uint16_t value = 0;
    std::memcpy(&value, rdram + (offset_of(address) ^ 2u), sizeof(value));
    return value;
}

uint8_t& byte_at(uint8_t* rdram, uint32_t address) {
    return rdram[offset_of(address) ^ 3u];
}

// One log line per cheat each time it starts changing the game, so a run's log
// shows when a cheat took effect without a line per frame.
bool g_goth_logged = false;
bool g_items_logged = false;

void hold_goth(uint8_t* rdram) {
    const uint32_t goth = load_u32(rdram, kGothAddress);
    if (goth == kGothMax) {
        return;
    }
    store_u32(rdram, kGothAddress, kGothMax);
    if (!g_goth_logged) {
        std::fprintf(stderr, "[cheats] max goth: %u -> %u\n", goth, kGothMax);
        g_goth_logged = true;
    }
}

void hold_item_counts(uint8_t* rdram) {
    int raised = 0;
    for (const ItemList& list : kItemLists) {
        for (int slot = 0; slot < list.slots; ++slot) {
            const uint32_t entry = list.address + uint32_t(slot) * kItemSlotSize;
            if (load_u16(rdram, entry) == 0) {
                continue;  // empty slot
            }
            uint8_t& count = byte_at(rdram, entry + kItemCountOffset);
            if (count != kItemMax) {
                count = kItemMax;
                ++raised;
            }
        }
    }
    if (raised > 0 && !g_items_logged) {
        std::fprintf(stderr, "[cheats] max item counts: %d slot(s) set to %u\n", raised,
                     unsigned(kItemMax));
        g_items_logged = true;
    }
}

}  // namespace

void cheats_tick() {
    const bool goth = cheat_max_goth();
    const bool items = cheat_max_items();
    if (!goth) {
        g_goth_logged = false;
    }
    if (!items) {
        g_items_logged = false;
    }
    if (!goth && !items) {
        return;
    }
    uint8_t* rdram = ultramodern::get_rdram_base();
    if (rdram == nullptr) {
        return;
    }
    if (goth) {
        hold_goth(rdram);
    }
    if (items) {
        hold_item_counts(rdram);
    }
}

}  // namespace ogre
