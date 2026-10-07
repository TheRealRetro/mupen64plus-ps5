// Mupen64Plus PS5: sound through libSceAudioOut (S16 stereo, 48 kHz, 256-frame grains, own thread), as in
// PS5SX2's ProsperoAudio. The audio plugin (n64/plugins/audio_ps5.cpp) resamples to 48 kHz, so this is only a ring buffer.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace ps5audio
{
constexpr int kRate = 48000;
constexpr int kGrain = 256; // frames per sceAudioOutOutput
constexpr int kCapacity = 8192; // ring size in frames (~170 ms; a power of two)

bool Init();
void Shutdown();

// Producer side (the emulation thread). Frames that don't fit are dropped. Returns frames written.
int Push(const int16_t* stereo, int frames);
int Free(); // frames that fit right now
int Queued(); // frames waiting to be played
// Underruns since the start (the audio thread played silence because the ring was empty).
uint64_t Underruns();
} // namespace ps5audio
