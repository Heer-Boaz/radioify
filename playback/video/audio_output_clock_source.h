#pragma once

#include <cstdint>

#include "playback_main_clock.h"

namespace playback_audio_output_clock_source {

playback_main_clock::AudioClockStatus sample(bool audioActive, int64_t nowUs,
                                             bool audioMayDriveMaster);

void prime(int serial, int64_t targetPtsUs);

uint64_t updateCounter();
uint64_t waitForUpdate(uint64_t lastCounter, int timeoutMs);

}  // namespace playback_audio_output_clock_source
