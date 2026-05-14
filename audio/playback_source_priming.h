#pragma once

#include <cstdint>

struct PlaybackSourcePriming {
  uint32_t capacityFrames = 0;
  uint32_t targetFrames = 0;
  uint32_t primeFrames = 0;
};

PlaybackSourcePriming playbackSourcePrimingForRate(uint32_t sampleRate);

bool playbackSourceIsPrimed(uint64_t bufferedFrames,
                            uint32_t primeFrames,
                            bool atEnd);
