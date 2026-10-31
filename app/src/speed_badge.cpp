// The game-speed badge (see speed_badge.hpp).
//
// Behaviour: the live multiplier (`settings.cpp`) is sampled once per frame.
// When it changes, a small borderless always-on-top window appears in the game
// window's lower-right corner showing "2x", "4x" and so on, and disappears
// again `kSpeedBadgeHoldMs` later. "1x" is drawn too, so stepping back down to
// normal speed is visible.
//
// The window is created on the first change, so a run that never touches the
// speed has no third SDL window at all.
//
// Three environment knobs, all off unless set:
//
//   * `OGRE_SPEED_KEYS="up,up,down"` and `OGRE_SPEED_KEYS_AT_MS=<n>`: deliver
//     that many synthetic `=` / `-` key presses, one every 150 ms, starting n
//     ms after init, through the same SDL event path a real press takes. This
//     is how a scripted run exercises the hotkeys.
//   * `OGRE_SPEED_PAD="r3,r3"` and `OGRE_SPEED_PAD_AT_MS=<n>`: the same for the
//     pad's R3, as controller button-down events. Both scripts measure their
//     delay from one clock started at init, and a frame delivers the key script
//     first when both are due.
//   * `OGRE_CAPTURE_BADGE=<path>`: write the badge window's pixels as a PPM,
//     one file per show, so the file holds the last badge the run put up. This
//     is the same proof `OGRE_CAPTURE_OVERLAY` gives for the panel.

#include "speed_badge.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <ultramodern/config.hpp>

#include "font.hpp"
#include "input_map.hpp"
#include "settings.hpp"
#include "ui.hpp"

namespace ogre {
namespace {

// How long the badge stays up after a change.
constexpr uint32_t kSpeedBadgeHoldMs = 2000;
// Text scale, padding inside the badge and the gap to the window's corner, all
// in the window's own pixels.
constexpr int kScale = 3;
constexpr int kPadding = 10;
constexpr int kMargin = 20;
// Uniform window opacity, so the badge sits on the game's picture rather than
// on a black box. SDL2 has no per-pixel window transparency.
constexpr float kOpacity = 0.85f;

const SDL_Color kBackground{10, 11, 14, 255};
const SDL_Color kBorder{198, 160, 92, 255};
const SDL_Color kTextColor{238, 238, 232, 255};

struct BadgeState {
    bool initialized = false;
    SDL_Window* game_window = nullptr;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    Font font;
    // The live multiplier as of the previous frame. -1 until the first frame
    // has read it, so the value a run starts on does not draw a badge.
    int last_speed = -1;
    std::string shown_text;
    uint64_t hide_at_ms = 0;
    bool up = false;
    bool fullscreen_reported = false;
    const char* shot_path = nullptr;
    bool shot_written = false;
    // OGRE_SPEED_KEYS: +1 is a speed-up press, -1 a speed-down press.
    std::vector<int> keys;
    size_t key_next = 0;
    uint32_t keys_at_ms = 1000;
    uint64_t keys_start_ms = 0;
    // OGRE_SPEED_PAD: SDL controller button codes, delivered as button-down
    // events so the app's own handler sees them.
    std::vector<int> pad_buttons;
    size_t pad_next = 0;
    uint32_t pad_at_ms = 1000;
};

BadgeState g_badge;

std::string speed_text(int multiplier) {
    return std::to_string(multiplier) + "x";
}

uint32_t env_millis(const char* name, uint32_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    const long parsed = std::strtol(value, nullptr, 0);
    return parsed > 0 ? static_cast<uint32_t>(parsed) : 0;
}

// "up,up,down,+,-": the `OGRE_SPEED_KEYS` script. Unknown names are reported
// and skipped, as in the overlay's own key script.
std::vector<int> parse_key_list(const char* spec) {
    std::vector<int> steps;
    if (spec == nullptr) {
        return steps;
    }
    const std::string text = spec;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t comma = text.find(',', pos);
        std::string name = text.substr(pos, comma == std::string::npos ? std::string::npos
                                                                       : comma - pos);
        const size_t first = name.find_first_not_of(" \t");
        const size_t last = name.find_last_not_of(" \t");
        if (first != std::string::npos) {
            name = name.substr(first, last - first + 1);
        }
        if (!name.empty()) {
            if (name == "up" || name == "+" || name == "=" || name == "plus" ||
                name == "equals") {
                steps.push_back(1);
            }
            else if (name == "down" || name == "-" || name == "minus") {
                steps.push_back(-1);
            }
            else {
                std::fprintf(stderr, "[badge] OGRE_SPEED_KEYS: unknown step '%s'\n", name.c_str());
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return steps;
}

// "r3,r3": the `OGRE_SPEED_PAD` script, as SDL controller button names from the
// CONTROLS tab's own table (`pad_button_from_name`), so the script and the panel
// spell a button the same way. `r3` is accepted as a short spelling of `rstick`.
std::vector<int> parse_pad_list(const char* spec) {
    std::vector<int> buttons;
    if (spec == nullptr) {
        return buttons;
    }
    const std::string text = spec;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t comma = text.find(',', pos);
        std::string name = text.substr(pos, comma == std::string::npos ? std::string::npos
                                                                       : comma - pos);
        const size_t first = name.find_first_not_of(" \t");
        const size_t last = name.find_last_not_of(" \t");
        if (first != std::string::npos) {
            name = name.substr(first, last - first + 1);
        }
        if (!name.empty()) {
            std::string lowered = name;
            for (char& c : lowered) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            int code = -1;
            if (lowered == "r3" || lowered == "rightstick" || lowered == "r-stick") {
                code = SDL_CONTROLLER_BUTTON_RIGHTSTICK;
            }
            else {
                code = pad_button_from_name(lowered);
            }
            if (code >= 0) {
                buttons.push_back(code);
            }
            else {
                std::fprintf(stderr, "[badge] OGRE_SPEED_PAD: unknown button '%s'\n",
                             name.c_str());
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return buttons;
}

// Whether the SETTINGS tab's WINDOW row put the game in fullscreen. The badge
// is a separate window over the game window, and fullscreen is a display space
// of its own on macOS (RT64 toggles the NSWindow), so the badge has nowhere to
// draw. The speed still changes; only the readout is missing.
bool game_is_fullscreen() {
    return ultramodern::renderer::get_graphics_config().wm_option ==
           ultramodern::renderer::WindowMode::Fullscreen;
}

bool create_window() {
    const uint32_t flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
                           SDL_WINDOW_SKIP_TASKBAR | SDL_WINDOW_ALLOW_HIGHDPI;
    // The size is set for the first text in `show`; anything non-zero works
    // here.
    g_badge.window = SDL_CreateWindow("Ogre Battle 64: Speed", 0, 0, 64, 32, flags);
    if (g_badge.window == nullptr) {
        std::fprintf(stderr, "[badge] window: %s\n", SDL_GetError());
        return false;
    }
    g_badge.renderer = SDL_CreateRenderer(g_badge.window, -1, SDL_RENDERER_ACCELERATED);
    if (g_badge.renderer == nullptr) {
        g_badge.renderer = SDL_CreateRenderer(g_badge.window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (g_badge.renderer == nullptr) {
        std::fprintf(stderr, "[badge] renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_badge.window);
        g_badge.window = nullptr;
        return false;
    }
    SDL_SetRenderDrawBlendMode(g_badge.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetWindowOpacity(g_badge.window, kOpacity);
    if (!g_badge.font.init(g_badge.renderer)) {
        std::fprintf(stderr, "[badge] font init failed\n");
        SDL_DestroyRenderer(g_badge.renderer);
        SDL_DestroyWindow(g_badge.window);
        g_badge.renderer = nullptr;
        g_badge.window = nullptr;
        return false;
    }
    SDL_HideWindow(g_badge.window);
    std::fprintf(stderr, "[badge] window ready\n");
    return true;
}

void hide() {
    if (g_badge.window == nullptr) {
        return;
    }
    if (g_badge.up) {
        SDL_HideWindow(g_badge.window);
        std::fprintf(stderr, "[badge] hidden\n");
        std::fflush(stderr);
    }
    g_badge.up = false;
}

// Places the badge in the game window's lower-right corner, `kMargin` in from
// both edges. Runs on every drawn frame, so the badge follows the game window
// while it is dragged or resized during its two seconds.
bool place(int width, int height) {
    SDL_Window* game_window = g_badge.game_window;
    if (game_window == nullptr || g_badge.window == nullptr) {
        return false;
    }
    int game_x = 0;
    int game_y = 0;
    int game_w = 0;
    int game_h = 0;
    SDL_GetWindowPosition(game_window, &game_x, &game_y);
    SDL_GetWindowSize(game_window, &game_w, &game_h);
    if (game_w <= 0 || game_h <= 0) {
        return false;
    }
    const int badge_x = game_x + game_w - width - kMargin;
    const int badge_y = game_y + game_h - height - kMargin;
    int current_x = 0;
    int current_y = 0;
    SDL_GetWindowPosition(g_badge.window, &current_x, &current_y);
    if (current_x != badge_x || current_y != badge_y) {
        SDL_SetWindowPosition(g_badge.window, badge_x, badge_y);
    }
    return true;
}

void show(const std::string& text) {
    g_badge.shown_text = text;
    if (g_badge.window == nullptr && !create_window()) {
        return;
    }
    const int width = g_badge.font.width(text, kScale) + 2 * kPadding;
    const int height = g_badge.font.height(kScale) + 2 * kPadding;
    SDL_SetWindowSize(g_badge.window, width, height);
    if (!place(width, height)) {
        return;
    }
    // Draw in the badge's own pixels; SDL scales to the drawable, which is what
    // keeps the text the same size on a high-DPI display.
    SDL_RenderSetLogicalSize(g_badge.renderer, width, height);
    SDL_ShowWindow(g_badge.window);
    // SDL_ShowWindow makes the badge the key window on macOS
    // (Cocoa_ShowWindow calls makeKeyAndOrderFront), which would deliver the
    // game's key events to it. Hand the keyboard straight back; the badge needs
    // none of it, and it stays on top because it is an always-on-top window.
    SDL_RaiseWindow(g_badge.game_window);
    g_badge.up = true;
    g_badge.hide_at_ms = SDL_GetTicks64() + kSpeedBadgeHoldMs;
    // OGRE_CAPTURE_BADGE writes one PPM per show, so the file holds the last
    // badge the run put up.
    g_badge.shot_written = false;
    int badge_x = 0;
    int badge_y = 0;
    SDL_GetWindowPosition(g_badge.window, &badge_x, &badge_y);
    int game_x = 0;
    int game_y = 0;
    int game_w = 0;
    int game_h = 0;
    SDL_GetWindowPosition(g_badge.game_window, &game_x, &game_y);
    SDL_GetWindowSize(g_badge.game_window, &game_w, &game_h);
    std::fprintf(stderr,
                 "[badge] %s shown for %ums at (%d,%d) %dx%d; game window (%d,%d) %dx%d\n",
                 text.c_str(), kSpeedBadgeHoldMs, badge_x, badge_y, width, height, game_x, game_y,
                 game_w, game_h);
    std::fflush(stderr);
}

void draw() {
    if (g_badge.renderer == nullptr || !g_badge.up) {
        return;
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(g_badge.window, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }
    place(width, height);
    const std::string& text = g_badge.shown_text;

    SDL_SetRenderDrawColor(g_badge.renderer, kBackground.r, kBackground.g, kBackground.b, 235);
    SDL_RenderClear(g_badge.renderer);
    SDL_SetRenderDrawColor(g_badge.renderer, kBorder.r, kBorder.g, kBorder.b, 255);
    const SDL_Rect border{0, 0, width, height};
    SDL_RenderDrawRect(g_badge.renderer, &border);

    const int text_width = g_badge.font.width(text, kScale);
    const int text_height = g_badge.font.height(kScale);
    g_badge.font.draw(g_badge.renderer, text, (width - text_width) / 2,
                      (height - text_height) / 2, kScale, kTextColor);

    // The capture reads the renderer before the present, as the overlay's own
    // `OGRE_CAPTURE_OVERLAY` does.
    if (g_badge.shot_path != nullptr && !g_badge.shot_written) {
        if (ui::write_ppm(g_badge.renderer, std::filesystem::path(g_badge.shot_path))) {
            std::fprintf(stderr, "[badge] wrote %s\n", g_badge.shot_path);
        }
        else {
            std::fprintf(stderr, "[badge] could not write %s: %s\n", g_badge.shot_path,
                         SDL_GetError());
        }
        g_badge.shot_written = true;
        std::fflush(stderr);
    }

    SDL_RenderPresent(g_badge.renderer);
}

// Delivers the next `OGRE_SPEED_KEYS` press, as a real `=` / `-` key event.
void deliver_script_key() {
    if (g_badge.key_next >= g_badge.keys.size() || g_badge.game_window == nullptr) {
        return;
    }
    const uint64_t elapsed = SDL_GetTicks64() - g_badge.keys_start_ms;
    if (elapsed < static_cast<uint64_t>(g_badge.keys_at_ms) + g_badge.key_next * 150) {
        return;
    }
    const bool up = g_badge.keys[g_badge.key_next++] > 0;
    SDL_Event synthetic{};
    synthetic.type = SDL_KEYDOWN;
    synthetic.key.state = SDL_PRESSED;
    synthetic.key.repeat = 0;
    synthetic.key.windowID = SDL_GetWindowID(g_badge.game_window);
    synthetic.key.keysym.scancode = up ? SDL_SCANCODE_EQUALS : SDL_SCANCODE_MINUS;
    synthetic.key.keysym.sym = up ? SDLK_EQUALS : SDLK_MINUS;
    SDL_PushEvent(&synthetic);
}

// Delivers the next `OGRE_SPEED_PAD` press, as a real controller button-down
// event. The app's handler for it lives in `sdl_platform.cpp`, so this script
// exercises the same path a pad does.
void deliver_script_pad() {
    if (g_badge.pad_next >= g_badge.pad_buttons.size() || g_badge.game_window == nullptr) {
        return;
    }
    const uint64_t elapsed = SDL_GetTicks64() - g_badge.keys_start_ms;
    if (elapsed < static_cast<uint64_t>(g_badge.pad_at_ms) + g_badge.pad_next * 150) {
        return;
    }
    SDL_Event synthetic{};
    synthetic.type = SDL_CONTROLLERBUTTONDOWN;
    synthetic.cbutton.state = SDL_PRESSED;
    synthetic.cbutton.button = static_cast<Uint8>(g_badge.pad_buttons[g_badge.pad_next++]);
    SDL_PushEvent(&synthetic);
}

}  // namespace

void speed_badge_init(SDL_Window* game_window) {
    g_badge.game_window = game_window;
    g_badge.keys = parse_key_list(std::getenv("OGRE_SPEED_KEYS"));
    g_badge.keys_at_ms = env_millis("OGRE_SPEED_KEYS_AT_MS", 1000);
    g_badge.pad_buttons = parse_pad_list(std::getenv("OGRE_SPEED_PAD"));
    g_badge.pad_at_ms = env_millis("OGRE_SPEED_PAD_AT_MS", 1000);
    // One clock for both scripts, so a run that names both keeps their order.
    g_badge.keys_start_ms = SDL_GetTicks64();
    if (const char* path = std::getenv("OGRE_CAPTURE_BADGE");
        path != nullptr && path[0] != '\0') {
        g_badge.shot_path = path;
    }
    g_badge.initialized = true;
}

void speed_badge_tick() {
    if (!g_badge.initialized) {
        return;
    }
    deliver_script_key();
    deliver_script_pad();

    const int speed = game_speed();
    if (g_badge.last_speed < 0) {
        // The first frame only records the value the run started on; a badge at
        // boot would be noise.
        g_badge.last_speed = speed;
        return;
    }
    if (speed != g_badge.last_speed) {
        g_badge.last_speed = speed;
        if (game_is_fullscreen()) {
            if (!g_badge.fullscreen_reported) {
                g_badge.fullscreen_reported = true;
                std::fprintf(stderr,
                             "[badge] fullscreen: the renderer owns the display, so the speed "
                             "readout is not drawn\n");
                std::fflush(stderr);
            }
        }
        else {
            show(speed_text(speed));
        }
    }

    if (!g_badge.up) {
        return;
    }
    // SDL2's window hit test has no click-through result, so the pointer can
    // land on the badge window and a click there would be swallowed; get out of
    // the way instead.
    if (g_badge.window != nullptr && SDL_GetMouseFocus() == g_badge.window) {
        std::fprintf(stderr, "[badge] hidden: the pointer is over the badge\n");
        std::fflush(stderr);
        hide();
        return;
    }
    if (SDL_GetTicks64() >= g_badge.hide_at_ms) {
        hide();
        return;
    }
    draw();
}

void speed_badge_message(const char* text) {
    if (!g_badge.initialized || text == nullptr) {
        return;
    }
    if (game_is_fullscreen()) {
        std::fprintf(stderr, "[badge] fullscreen: \"%s\" is not drawn\n", text);
        std::fflush(stderr);
        return;
    }
    show(text);
}

void speed_badge_shutdown() {
    if (g_badge.renderer != nullptr) {
        SDL_DestroyRenderer(g_badge.renderer);
        g_badge.renderer = nullptr;
    }
    if (g_badge.window != nullptr) {
        SDL_DestroyWindow(g_badge.window);
        g_badge.window = nullptr;
    }
    g_badge.up = false;
    g_badge.initialized = false;
}

SDL_Window* speed_badge_window() {
    return g_badge.window;
}

}  // namespace ogre
