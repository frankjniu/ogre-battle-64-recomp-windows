// Audio output (see audio_out.cpp): the game's AI buffers go into a FIFO that
// the app owns, and the output callback drains it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ogre::audio_out {

// Opens the output at `frequency` Hz, stereo signed 16-bit, and drops any
// queued samples. ultramodern calls it at boot and from the game's
// `osAiSetFrequency`. With OGRE_NO_AUDIO set it opens no device; with
// OGRE_AUDIO_SINK also set it starts the real-time sink instead.
void open(uint32_t frequency);
// Closes the device or stops the sink.
void close();
// Appends one AI buffer: `count` int16 samples, interleaved stereo, read from
// the game's RDRAM.
void queue_samples(const int16_t* samples, size_t count);
// Samples (both channels) waiting in the FIFO. `ultramodern::audio_dma_busy`
// and `osAiGetLength` read it through `get_frames_remaining`.
size_t queued_samples();
// The game's rate from the last `open` call.
uint32_t frequency();
// Main-thread poll for OGRE_AUDIO_MARK_KEY; called from the SDL event pump.
void tick();

}  // namespace ogre::audio_out
