// Quick save (F5) and quick load (F9) -- see quick_save.hpp.

#include "quick_save.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <ultramodern/ultramodern.hpp>

#include "speed_badge.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

// librecomp's save buffer writer (librecomp/src/pi.cpp). It is not in a public
// header; librecomp's own flash.cpp declares it the same way. It copies `count`
// bytes into the battery image under the buffer's mutex and wakes the saving
// thread, which writes `<config>/saves/<game id>.bin` (keeping a `.bak`).
void save_write_ptr(const void* in, uint32_t offset, uint32_t count);

namespace ogre {
namespace {

constexpr uint64_t kConfirmMs = 3000;
// How long a quick load waits for the saving thread to write the battery file.
constexpr uint64_t kBatteryWriteWaitMs = 2000;

// F9 waits for a second press within kConfirmMs (0 = not armed).
uint64_t g_load_armed_until = 0;
// Set by a quick load that restored the battery; main.cpp then ends the run and
// relaunches.
bool g_restart = false;

struct ScriptedKey {
    uint64_t at_ms;
    SDL_Keycode key;
};
std::vector<ScriptedKey> g_script;
size_t g_script_next = 0;
uint64_t g_script_start = 0;
bool g_script_parsed = false;

std::filesystem::path battery_path() {
    return ultramodern::get_save_file_path();
}

std::filesystem::path slot_path() {
    const std::filesystem::path battery = battery_path();
    return battery.parent_path() / (battery.stem().string() + "-quick" + battery.extension().string());
}

bool read_file(const std::filesystem::path& path, std::vector<char>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Writes `<path>.tmp` and renames it over `path`, so a crash mid-write leaves
// the previous slot intact.
bool write_file(const std::filesystem::path& path, const std::vector<char>& data) {
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            return false;
        }
        out.write(data.data(), std::streamsize(data.size()));
        if (!out.good()) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

bool all_zero(const std::vector<char>& data) {
    for (char c : data) {
        if (c != 0) {
            return false;
        }
    }
    return true;
}

void message(const char* text) {
    speed_badge_message(text);
}

void quick_save() {
    std::vector<char> battery;
    if (!read_file(battery_path(), battery) || battery.empty() || all_zero(battery)) {
        std::fprintf(stderr, "[quicksave] F5: no battery save at %s yet; save in the game first\n",
                     battery_path().string().c_str());
        message("NO SAVE YET");
        return;
    }
    if (!write_file(slot_path(), battery)) {
        std::fprintf(stderr, "[quicksave] F5: could not write %s\n", slot_path().string().c_str());
        message("QUICK SAVE FAILED");
        return;
    }
    std::fprintf(stderr, "[quicksave] F5: %s -> %s (%zu bytes)\n", battery_path().string().c_str(),
                 slot_path().string().c_str(), battery.size());
    message("QUICK SAVED");
}

void quick_load() {
    std::vector<char> slot;
    if (!read_file(slot_path(), slot) || slot.empty()) {
        std::fprintf(stderr, "[quicksave] F9: no quick save at %s\n", slot_path().string().c_str());
        message("NO QUICK SAVE");
        return;
    }
    std::vector<char> battery;
    if (read_file(battery_path(), battery) && battery.size() != slot.size()) {
        std::fprintf(stderr, "[quicksave] F9: %s is %zu bytes, the battery is %zu; not loaded\n",
                     slot_path().string().c_str(), slot.size(), battery.size());
        message("QUICK SAVE DOES NOT FIT");
        return;
    }
    save_write_ptr(slot.data(), 0, uint32_t(slot.size()));
    // The saving thread writes the file within ~10 ms of the last write; the
    // restart must not run before it has.
    const uint64_t deadline = SDL_GetTicks64() + kBatteryWriteWaitMs;
    bool written = false;
    while (!written && SDL_GetTicks64() < deadline) {
        SDL_Delay(10);
        written = read_file(battery_path(), battery) && battery == slot;
    }
    if (!written) {
        std::fprintf(stderr, "[quicksave] F9: the battery file did not match the quick save "
                             "within %llu ms; not restarting\n",
                     (unsigned long long)kBatteryWriteWaitMs);
        message("QUICK LOAD FAILED");
        return;
    }
    g_restart = true;
    std::fprintf(stderr, "[quicksave] F9: %s -> %s (%zu bytes); restarting\n",
                 slot_path().string().c_str(), battery_path().string().c_str(), slot.size());
    message("LOADED - RESTARTING");
}

void parse_script() {
    g_script_parsed = true;
    const char* spec = std::getenv("OGRE_QUICK_KEYS");
    if (spec == nullptr) {
        return;
    }
    std::string text = spec;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t comma = text.find(',', pos);
        const std::string item = text.substr(pos, comma == std::string::npos ? std::string::npos
                                                                            : comma - pos);
        const size_t colon = item.find(':');
        if (colon != std::string::npos) {
            const uint64_t at = std::strtoull(item.c_str(), nullptr, 10);
            const std::string name = item.substr(colon + 1);
            if (name == "f5" || name == "F5") {
                g_script.push_back({at, SDLK_F5});
            }
            else if (name == "f9" || name == "F9") {
                g_script.push_back({at, SDLK_F9});
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    g_script_start = SDL_GetTicks64();
}

// Delivers `OGRE_QUICK_KEYS` presses as real key events, so a test run goes
// through the event pump's own conditions.
void deliver_script() {
    if (!g_script_parsed) {
        parse_script();
    }
    while (g_script_next < g_script.size() &&
           SDL_GetTicks64() - g_script_start >= g_script[g_script_next].at_ms) {
        SDL_Event synthetic{};
        synthetic.type = SDL_KEYDOWN;
        synthetic.key.state = SDL_PRESSED;
        synthetic.key.repeat = 0;
        synthetic.key.keysym.sym = g_script[g_script_next].key;
        synthetic.key.keysym.scancode =
            g_script[g_script_next].key == SDLK_F5 ? SDL_SCANCODE_F5 : SDL_SCANCODE_F9;
        SDL_PushEvent(&synthetic);
        ++g_script_next;
    }
}

}  // namespace

bool quick_save_handle_key(SDL_Keycode key) {
    if (key == SDLK_F5) {
        quick_save();
        return true;
    }
    if (key != SDLK_F9) {
        return false;
    }
    const uint64_t now = SDL_GetTicks64();
    if (g_load_armed_until != 0 && now <= g_load_armed_until) {
        g_load_armed_until = 0;
        quick_load();
    }
    else {
        g_load_armed_until = now + kConfirmMs;
        std::fprintf(stderr, "[quicksave] F9: press again within %llu ms to load\n",
                     (unsigned long long)kConfirmMs);
        message("PRESS F9 AGAIN TO LOAD");
    }
    return true;
}

void quick_save_tick() {
    deliver_script();
    if (g_load_armed_until != 0 && SDL_GetTicks64() > g_load_armed_until) {
        g_load_armed_until = 0;
    }
}

bool quick_save_restart_requested() {
    return g_restart;
}

void quick_save_relaunch() {
#if defined(_WIN32)
    // The child inherits this environment; OGRE_RELAUNCH=1 makes it boot past
    // the start screen, and main.cpp clears it in the child.
    SetEnvironmentVariableW(L"OGRE_RELAUNCH", L"1");
    // A scripted test run must not press F9 again in the new process.
    SetEnvironmentVariableW(L"OGRE_QUICK_KEYS", nullptr);
    const std::wstring command = GetCommandLineW();
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &startup, &process)) {
        std::fprintf(stderr, "[quicksave] restarted as pid %lu\n",
                     (unsigned long)process.dwProcessId);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    else {
        std::fprintf(stderr, "[quicksave] restart failed (error %lu); start the game again\n",
                     (unsigned long)GetLastError());
    }
#else
    std::fprintf(stderr, "[quicksave] the quick save is the battery now; start the game again\n");
#endif
    std::fflush(stderr);
}

}  // namespace ogre
