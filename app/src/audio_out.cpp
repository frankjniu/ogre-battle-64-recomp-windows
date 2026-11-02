// Audio output.
//
// The game's audio thread hands each finished AI buffer to `osAiSetNextBuffer`;
// ultramodern passes it to `queue_samples`, which appends it to a FIFO. The
// output callback drains the FIFO one device chunk at a time. A pull that
// finds fewer samples than one chunk is an underrun: the output breaks into
// silence, and the listener hears a click. After an underrun the output plays
// silence until the FIFO holds the refill threshold again (see
// kRefillDefaultMs; `OGRE_AUDIO_REFILL_MS=0` turns it off and plays partial
// chunks padded with silence, as SDL's queue did).
//
// The app owns the FIFO so that it can count underruns. SDL's own queue
// (`SDL_QueueAudio`) pads a short read with silence and reports nothing.
//
// Diagnostics:
// - `OGRE_AUDIO_STATS=1` prints one `[audio]` line per second: AI buffers
//   pushed, device pulls, underruns and the silence played, the FIFO depth
//   each pull found, and the longest gap between two pushes.
// - `OGRE_AUDIO_SINK=<path>` with `OGRE_NO_AUDIO=1` opens no device. A thread
//   drains the FIFO on a real-time clock in SDL's WASAPI pattern and writes
//   what it pulled to <path> (raw S16LE stereo at the game's rate). The game's
//   pacing then sees the queue a device would leave, so a run without sound
//   can measure underruns.
// - `OGRE_AUDIO_MARK_KEY=<1..9>`: each press of that digit prints
//   `[audio] mark <n>`, so a listener can stamp the moment of a click into the
//   same log as the stats lines.
//
// Rate conversion. The game sets 32000 Hz (`osAiSetFrequency`). By default the
// device is opened at that rate, and SDL's WASAPI backend passes the rate to
// Windows with AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, so the Windows audio engine
// converts to the mix rate (48000 Hz on the developer's devices).
// `OGRE_AUDIO_RESAMPLE=app` opens the device at the default device's own rate
// instead and converts in the callback with SDL's resampler (`SDL_AudioStream`).

#include "audio_out.hpp"

#include "sdl_platform.hpp"
#include "settings.hpp"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace ogre::audio_out {

namespace {

using Clock = std::chrono::steady_clock;

// The device callback's size in stereo frames: 16 ms at 32 kHz.
constexpr int kChunkFrames = 512;

// A ring of int16 samples that grows when a push does not fit. Guarded by
// g_mutex. The buffer is never empty, so the modulo in `pop` is defined.
struct Fifo {
    std::vector<int16_t> buf = std::vector<int16_t>(size_t{1} << 16);
    size_t head = 0;  // index of the oldest sample
    size_t size = 0;

    void clear() {
        head = 0;
        size = 0;
    }

    // Moves up to `n` samples to `dst` and returns how many it moved.
    size_t pop(int16_t* dst, size_t n) {
        n = std::min(n, size);
        const size_t first = std::min(n, buf.size() - head);
        std::memcpy(dst, buf.data() + head, first * sizeof(int16_t));
        std::memcpy(dst + first, buf.data(), (n - first) * sizeof(int16_t));
        head = (head + n) % buf.size();
        size -= n;
        return n;
    }

    void push(const int16_t* src, size_t n) {
        if (size + n > buf.size()) {
            size_t capacity = buf.size();
            while (capacity < size + n) {
                capacity *= 2;
            }
            std::vector<int16_t> grown(capacity);
            const size_t kept = size;
            pop(grown.data(), kept);
            buf.swap(grown);
            head = 0;
            size = kept;
        }
        const size_t tail = (head + size) % buf.size();
        const size_t first = std::min(n, buf.size() - tail);
        std::memcpy(buf.data() + tail, src, first * sizeof(int16_t));
        std::memcpy(buf.data(), src + first, (n - first) * sizeof(int16_t));
        size += n;
    }
};

// Counters for one OGRE_AUDIO_STATS window. Guarded by g_mutex.
struct Window {
    Clock::time_point start = Clock::now();
    uint32_t pushes = 0;
    uint64_t pushed_samples = 0;
    size_t push_min = std::numeric_limits<size_t>::max();
    size_t push_max = 0;
    double push_gap_max_ms = 0;
    uint32_t pulls = 0;
    uint32_t underruns = 0;       // pulls that found too few samples while playing
    uint64_t silent_samples = 0;  // silence played while starved or refilling
    size_t depth_min = std::numeric_limits<size_t>::max();  // FIFO at a pull
    size_t depth_max = 0;
};

// Refill threshold (OGRE_AUDIO_REFILL_MS, default 40 ms). After the FIFO runs
// dry, and at open, the output plays silence until the FIFO holds this much,
// then plays continuously. Without it, a FIFO that runs dry stays near empty
// while the game catches up: the game pushes at most one 552-frame buffer per
// retrace (33 120 frames/s against the DAC's 32 000), so the climb back takes
// seconds and every pull in it finds less than a chunk. Measured with the sink
// at boot: 6 partial pulls in 3 s, each a click. With the threshold the same
// starvation is one gap. It must stay below the runtime's 52 ms busy cushion
// (`ultramodern::audio_dma_busy`): the game stops producing above the cushion,
// so a higher threshold would never be reached.
constexpr uint32_t kRefillDefaultMs = 40;
constexpr uint32_t kRefillMaxMs = 48;

std::mutex g_mutex;
Fifo g_fifo;
Window g_window;
Clock::time_point g_last_push;
bool g_have_push = false;
bool g_refilling = true;
size_t g_refill_samples = 0;
uint64_t g_total_underruns = 0;
uint64_t g_total_silent = 0;

const Clock::time_point g_t0 = Clock::now();
const bool g_stats_enabled = std::getenv("OGRE_AUDIO_STATS") != nullptr;

// True while a device or the sink drains the FIFO; `queue_samples` drops
// buffers otherwise, as the SDL queue path did with no device.
std::atomic<bool> g_active{false};
std::atomic<uint32_t> g_frequency{0};

// Owned by open/close, which take g_open_mutex.
std::mutex g_open_mutex;
SDL_AudioDeviceID g_device = 0;
std::thread g_sink;
std::atomic<bool> g_sink_stop{false};

// OGRE_AUDIO_RESAMPLE=app: game rate -> device rate. Created by `open` before
// the device starts and used only by the device callback after that.
SDL_AudioStream* g_resampler = nullptr;
uint32_t g_device_rate = 0;
std::vector<int16_t> g_resample_in;

double samples_to_ms(uint64_t samples, uint32_t frequency) {
    if (frequency == 0) {
        return 0.0;
    }
    return 1000.0 * static_cast<double>(samples) / 2.0 / frequency;
}

void print_window(const Window& w, uint64_t total_underruns, uint64_t total_silent,
                  uint32_t frequency) {
    const double t = std::chrono::duration<double>(Clock::now() - g_t0).count();
    const size_t depth_min = w.pulls ? w.depth_min : 0;
    const size_t push_min = w.pushes ? w.push_min : 0;
    fprintf(stderr,
            "[audio] t=%.1fs push=%u (%zu..%zu frames, gap max %.1fms) pull=%u under=%u "
            "silence=%.1fms fifo=%.1f..%.1fms | total under=%llu silence=%.1fms\n",
            t, w.pushes, push_min / 2, w.push_max / 2, w.push_gap_max_ms, w.pulls, w.underruns,
            samples_to_ms(w.silent_samples, frequency), samples_to_ms(depth_min, frequency),
            samples_to_ms(w.depth_max, frequency),
            static_cast<unsigned long long>(total_underruns),
            samples_to_ms(total_silent, frequency));
    fflush(stderr);
}

// Fills `out` with `n` samples from the FIFO, or with silence while the FIFO
// refills, then applies the SOUNDS/VOLUME gain. Runs on the device's thread
// (or the sink's).
void pull(int16_t* out, size_t n) {
    size_t got = 0;
    bool print = false;
    Window done;
    uint64_t total_underruns = 0;
    uint64_t total_silent = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Window& w = g_window;
        const size_t depth = g_fifo.size;
        if (g_refilling && depth >= g_refill_samples) {
            g_refilling = false;
        }
        if (!g_refilling) {
            if (depth < n && g_refill_samples > 0) {
                // Starved: keep what is queued and play silence until the FIFO
                // holds the refill threshold again.
                g_refilling = true;
                w.underruns++;
                g_total_underruns++;
            } else {
                got = g_fifo.pop(out, n);
                if (got < n) {
                    w.underruns++;
                    g_total_underruns++;
                }
            }
        }
        w.pulls++;
        w.depth_min = std::min(w.depth_min, depth);
        w.depth_max = std::max(w.depth_max, depth);
        w.silent_samples += n - got;
        g_total_silent += n - got;
        if (g_stats_enabled && Clock::now() - w.start >= std::chrono::seconds(1)) {
            done = w;
            g_window = Window{};
            total_underruns = g_total_underruns;
            total_silent = g_total_silent;
            print = true;
        }
    }
    if (got < n) {
        std::memset(out + got, 0, (n - got) * sizeof(int16_t));
    }
    const int gain = audio_gain_percent();
    if (gain < 100) {
        for (size_t i = 0; i < got; i++) {
            out[i] = static_cast<int16_t>(static_cast<int32_t>(out[i]) * gain / 100);
        }
    }
    if (print) {
        print_window(done, total_underruns, total_silent,
                     g_frequency.load(std::memory_order_relaxed));
    }
}

void SDLCALL sdl_fill(void*, Uint8* stream, int len) {
    pull(reinterpret_cast<int16_t*>(stream), static_cast<size_t>(len) / sizeof(int16_t));
}

// The device runs at g_device_rate. Each pass pulls the game-rate frames that
// the missing output needs, plus one, and feeds them to the resampler; the
// resampler keeps its filter history across calls.
void SDLCALL sdl_fill_resampled(void*, Uint8* stream, int len) {
    constexpr int kFrameBytes = 2 * static_cast<int>(sizeof(int16_t));
    const uint32_t in_rate = g_frequency.load(std::memory_order_relaxed);
    int available = SDL_AudioStreamAvailable(g_resampler);
    while (available >= 0 && available < len) {
        const uint64_t missing_frames = static_cast<uint64_t>(len - available) / kFrameBytes;
        const size_t in_frames =
            static_cast<size_t>(missing_frames * in_rate / g_device_rate) + 1;
        g_resample_in.resize(in_frames * 2);
        pull(g_resample_in.data(), g_resample_in.size());
        if (SDL_AudioStreamPut(g_resampler, g_resample_in.data(),
                               static_cast<int>(in_frames) * kFrameBytes) != 0) {
            break;
        }
        available = SDL_AudioStreamAvailable(g_resampler);
    }
    const int got = SDL_AudioStreamGet(g_resampler, stream, len);
    if (got < len) {
        std::memset(stream + std::max(got, 0), 0, static_cast<size_t>(len - std::max(got, 0)));
    }
}

// SDL's WASAPI path, reproduced on a clock: the device takes one 10 ms period
// at a time, and SDL pulls one 512-frame chunk from the callback whenever its
// converter holds less than a period (SDL_RunAudio in SDL_audio.c).
void sink_main(uint32_t frequency, FILE* file) {
    const size_t period_frames = frequency / 100;
    const auto period = std::chrono::nanoseconds(
        static_cast<uint64_t>(period_frames) * 1000000000ull / frequency);
    std::vector<int16_t> chunk(static_cast<size_t>(kChunkFrames) * 2);
    size_t converter = 0;
    auto next = Clock::now();
    while (!g_sink_stop.load(std::memory_order_relaxed)) {
        while (converter < period_frames) {
            pull(chunk.data(), chunk.size());
            if (file != nullptr) {
                fwrite(chunk.data(), sizeof(int16_t), chunk.size(), file);
                fflush(file);
            }
            converter += kChunkFrames;
        }
        converter -= period_frames;
        next += period;
        std::this_thread::sleep_until(next);
    }
    if (file != nullptr) {
        fclose(file);
    }
}

void close_locked() {
    g_active.store(false);
    if (g_device != 0) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
    if (g_resampler != nullptr) {
        SDL_FreeAudioStream(g_resampler);
        g_resampler = nullptr;
    }
    if (g_sink.joinable()) {
        g_sink_stop.store(true);
        g_sink.join();
        g_sink_stop.store(false);
    }
}

}  // namespace

void open(uint32_t frequency) {
    std::lock_guard<std::mutex> open_lock(g_open_mutex);
    close_locked();
    g_frequency.store(frequency);
    static const uint32_t refill_ms = [] {
        const char* v = std::getenv("OGRE_AUDIO_REFILL_MS");
        const uint32_t ms =
            (v != nullptr) ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : kRefillDefaultMs;
        if (ms > kRefillMaxMs) {
            fprintf(stderr, "[audio] OGRE_AUDIO_REFILL_MS=%u is above the busy cushion; using %u\n",
                    ms, kRefillMaxMs);
            return kRefillMaxMs;
        }
        return ms;
    }();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_fifo.clear();
        g_window = Window{};
        g_have_push = false;
        g_refilling = true;
        g_refill_samples = static_cast<size_t>(frequency) * refill_ms / 1000 * 2;
    }
    // OGRE_NO_AUDIO=1: skip opening an output device entirely. SDL's
    // SDL_OpenAudioDevice() blocks indefinitely on some hosts (observed on
    // macOS when no output device is usable), and because the runtime calls it
    // from the game start thread's preinit(), that stalls the whole boot before
    // a single recompiled function runs. Audio is not needed for rendering
    // diagnostics, so allow a run to proceed silently without it.
    if (std::getenv("OGRE_NO_AUDIO") != nullptr) {
        const char* sink = std::getenv("OGRE_AUDIO_SINK");
        if (sink == nullptr || sink[0] == '\0') {
            fprintf(stderr, "[SDL] OGRE_NO_AUDIO: no output device opened\n");
            return;
        }
        FILE* file = std::fopen(sink, "wb");
        if (file == nullptr) {
            fprintf(stderr, "[audio] OGRE_AUDIO_SINK: cannot write %s; draining without a file\n", sink);
        }
        fprintf(stderr, "[audio] OGRE_NO_AUDIO with OGRE_AUDIO_SINK: real-time sink at %u Hz\n",
                frequency);
        g_active.store(true);
        g_sink = std::thread(sink_main, frequency, file);
        return;
    }
    g_device_rate = frequency;
    const char* resample = std::getenv("OGRE_AUDIO_RESAMPLE");
    if (resample != nullptr && std::strcmp(resample, "app") == 0) {
#if SDL_VERSION_ATLEAST(2, 24, 0)
        SDL_AudioSpec info{};
        if (SDL_GetDefaultAudioInfo(nullptr, &info, 0) == 0 && info.freq > 0) {
            g_device_rate = static_cast<uint32_t>(info.freq);
        }
#endif
    }
    if (g_device_rate != frequency) {
        g_resampler = SDL_NewAudioStream(AUDIO_S16SYS, 2, static_cast<int>(frequency), AUDIO_S16SYS,
                                         2, static_cast<int>(g_device_rate));
        if (g_resampler == nullptr) {
            fprintf(stderr, "[audio] SDL_NewAudioStream failed: %s\n", SDL_GetError());
            g_device_rate = frequency;
        }
    }
    SDL_AudioSpec want{};
    want.freq = static_cast<int>(g_device_rate);
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = kChunkFrames;
    want.callback = g_resampler != nullptr ? sdl_fill_resampled : sdl_fill;
    g_device = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (g_device == 0) {
        fprintf(stderr, "[SDL] Failed to open audio device: %s\n", SDL_GetError());
        if (g_resampler != nullptr) {
            SDL_FreeAudioStream(g_resampler);
            g_resampler = nullptr;
        }
        return;
    }
    fprintf(stderr, "[audio] device opened at %u Hz for the game's %u Hz (%s)\n", g_device_rate,
            frequency,
            g_resampler != nullptr ? "the app resamples" : "the audio driver converts the rate");
    g_active.store(true);
    SDL_PauseAudioDevice(g_device, 0);
}

void tick() {
#if !defined(__EMSCRIPTEN__)
    static const int key = [] {
        const char* v = std::getenv("OGRE_AUDIO_MARK_KEY");
        return (v != nullptr) ? static_cast<int>(std::strtol(v, nullptr, 0)) : 0;
    }();
    if (key == 0) {
        return;
    }
    static bool was_down = false;
    static int marks = 0;
    const bool down = console::key_down(key);
    if (down && !was_down) {
        const double t = std::chrono::duration<double>(Clock::now() - g_t0).count();
        fprintf(stderr, "[audio] mark %d t=%.1fs\n", ++marks, t);
        fflush(stderr);
    }
    was_down = down;
#endif
}

void close() {
    std::lock_guard<std::mutex> open_lock(g_open_mutex);
    close_locked();
}

void queue_samples(const int16_t* samples, size_t count) {
    if (!g_active.load() || samples == nullptr || count == 0) {
        return;
    }
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fifo.push(samples, count);
    Window& w = g_window;
    if (g_have_push) {
        const double gap = std::chrono::duration<double, std::milli>(now - g_last_push).count();
        w.push_gap_max_ms = std::max(w.push_gap_max_ms, gap);
    }
    g_have_push = true;
    g_last_push = now;
    w.pushes++;
    w.pushed_samples += count;
    w.push_min = std::min(w.push_min, count);
    w.push_max = std::max(w.push_max, count);
}

size_t queued_samples() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_fifo.size;
}

uint32_t frequency() {
    return g_frequency.load();
}

}  // namespace ogre::audio_out
