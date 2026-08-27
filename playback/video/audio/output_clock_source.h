#pragma once

#include <cstdint>

#include "playback/video/timing/main_clock.h"

class AudioPlaybackRuntime;

namespace playback_audio_output_clock_source {

playback_video_main_clock::AudioClockStatus sample(
    const AudioPlaybackRuntime& audioPlayback, bool audioActive,
    int64_t nowUs, bool audioMayDriveMaster);

void prime(AudioPlaybackRuntime& audioPlayback, int serial,
           int64_t targetPtsUs);

uint64_t updateCounter(const AudioPlaybackRuntime& audioPlayback);
uint64_t waitForUpdate(const AudioPlaybackRuntime& audioPlayback,
                       uint64_t lastCounter, int timeoutMs);

}  // namespace playback_audio_output_clock_source
