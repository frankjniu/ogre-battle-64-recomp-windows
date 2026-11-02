// Cheats from the CHEATS tab (see cheats.hpp).

#include "cheats.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include <librecomp/game.hpp>
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

// The class list's bitfield and the bytes the GameShark code writes there, in
// address order (`81196A96 F4FF` ... `80196AA0 0001`).
constexpr uint32_t kClassBitsAddress = 0x80196A96u;
constexpr uint8_t kClassBits[] = {0xF4, 0xFF, 0xFF, 0xFF, 0xFF, 0xEB,
                                  0xFF, 0xFF, 0xFF, 0xFF, 0x01};

// The low byte of the hour (u16 at 0x80196A4C).
constexpr uint32_t kHourByteAddress = 0x80196A4Du;

// The mission day counter in record 7's BSS, and record 7's image.
constexpr uint32_t kDayAddress = 0x801F108Cu;
constexpr uint32_t kMissionRom = 0x101D00u;
constexpr uint32_t kMissionRam = 0x801AD5C0u;

// RDRAM words that must equal record 7's ROM words for the day counter to be
// record 7's: the day advance in `func_ovlN_801B134C` (`0x801B141C`, 7 words)
// and the jump table `jtbl_ovlN_801F0A18` near the end of the image (4 words).
// Unit X's record 10c (RAM from `0x801E6FD0`) and unit S's record 06 cover the
// second range, so a match on both says no other bank has replaced the top of
// record 7.
struct Probe {
    uint32_t address;
    uint32_t words;
};
constexpr Probe kMissionProbes[] = {
    {0x801B141Cu, 7},
    {0x801F0A18u, 4},
};

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

void store_u16(uint8_t* rdram, uint32_t address, uint16_t value) {
    std::memcpy(rdram + (offset_of(address) ^ 2u), &value, sizeof(value));
}

uint8_t& byte_at(uint8_t* rdram, uint32_t address) {
    return rdram[offset_of(address) ^ 3u];
}

// One log line per cheat each time it starts changing the game, so a run's log
// shows when a cheat took effect without a line per frame. The hook flags are
// set on the game's threads and cleared here.
bool g_goth_logged = false;
bool g_items_logged = false;
bool g_classes_logged = false;
bool g_day_logged = false;
bool g_hour_logged = false;
std::atomic<bool> g_level_logged{false};
std::atomic<bool> g_promotion_logged{false};

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

void hold_classes(uint8_t* rdram) {
    int changed = 0;
    for (size_t i = 0; i < sizeof(kClassBits); ++i) {
        uint8_t& bits = byte_at(rdram, kClassBitsAddress + uint32_t(i));
        const uint8_t wanted = bits | kClassBits[i];
        if (bits != wanted) {
            bits = wanted;
            ++changed;
        }
    }
    if (changed > 0 && !g_classes_logged) {
        std::fprintf(stderr, "[cheats] unlock all classes: %d byte(s) of 0x%08X set\n", changed,
                     kClassBitsAddress);
        g_classes_logged = true;
    }
}

void hold_hour(uint8_t* rdram, int hour) {
    uint8_t& value = byte_at(rdram, kHourByteAddress);
    if (value == uint8_t(hour)) {
        return;
    }
    if (!g_hour_logged) {
        std::fprintf(stderr, "[cheats] time of day: hour %u -> %d\n", unsigned(value), hour);
        g_hour_logged = true;
    }
    value = uint8_t(hour);
}

uint32_t rom_word(std::span<const uint8_t> rom, uint32_t offset) {
    return (uint32_t(rom[offset]) << 24) | (uint32_t(rom[offset + 1]) << 16) |
           (uint32_t(rom[offset + 2]) << 8) | uint32_t(rom[offset + 3]);
}

bool mission_module_resident(const uint8_t* rdram) {
    const std::span<const uint8_t> rom = recomp::get_rom();
    for (const Probe& probe : kMissionProbes) {
        for (uint32_t i = 0; i < probe.words; ++i) {
            const uint32_t address = probe.address + 4 * i;
            const uint32_t rom_offset = kMissionRom + (address - kMissionRam);
            if (size_t(rom_offset) + 4 > rom.size() ||
                load_u32(rdram, address) != rom_word(rom, rom_offset)) {
                return false;
            }
        }
    }
    return true;
}

void hold_day_zero(uint8_t* rdram) {
    if (load_u16(rdram, kDayAddress) == 0 || !mission_module_resident(rdram)) {
        return;
    }
    if (!g_day_logged) {
        std::fprintf(stderr, "[cheats] day always 00: day %u -> 0\n",
                     unsigned(byte_at(rdram, kDayAddress)));
        g_day_logged = true;
    }
    store_u16(rdram, kDayAddress, 0);
}

}  // namespace

void cheats_tick() {
    const bool goth = cheat_enabled(Cheat::MaxGoth);
    const bool items = cheat_enabled(Cheat::MaxItems);
    const bool classes = cheat_enabled(Cheat::AllClasses);
    const bool day = cheat_enabled(Cheat::DayZero);
    const int hour = cheat_hour();
    g_goth_logged = goth && g_goth_logged;
    g_items_logged = items && g_items_logged;
    g_classes_logged = classes && g_classes_logged;
    g_day_logged = day && g_day_logged;
    g_hour_logged = hour >= 0 && g_hour_logged;
    if (!cheat_enabled(Cheat::QuickLevelGain)) {
        g_level_logged.store(false, std::memory_order_relaxed);
    }
    if (!cheat_enabled(Cheat::QuickPromotion)) {
        g_promotion_logged.store(false, std::memory_order_relaxed);
    }
    if (!goth && !items && !classes && !day && hour < 0) {
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
    if (classes) {
        hold_classes(rdram);
    }
    if (day) {
        hold_day_zero(rdram);
    }
    if (hour >= 0) {
        hold_hour(rdram, hour);
    }
}

}  // namespace ogre

extern "C" uint64_t ogre_cheat_level_check(uint64_t below_100) {
    if (!ogre::cheat_enabled(ogre::Cheat::QuickLevelGain)) {
        return below_100;
    }
    if (below_100 != 0 && !ogre::g_level_logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr, "[cheats] quick level gain: a unit below 100 experience levels up\n");
    }
    return 0;
}

extern "C" uint64_t ogre_cheat_promotion_counter(uint64_t counter, uint64_t points) {
    const int32_t earned = int32_t(points);
    if (!ogre::cheat_enabled(ogre::Cheat::QuickPromotion) || earned <= 0) {
        return counter;
    }
    const int32_t before = int32_t(counter);
    const int32_t after = before < 100 ? 100 : (before > 255 ? 255 : before);
    if (!ogre::g_promotion_logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[cheats] quick soldier promotion: counter %d (+%d points) -> %d\n",
                     before - earned, earned, after);
    }
    return uint64_t(int64_t(after));
}
