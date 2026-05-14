#include "playback_source_priming.h"

#include <algorithm>

PlaybackSourcePriming playbackSourcePrimingForRate(uint32_t sampleRate) {
  PlaybackSourcePriming priming{};
  priming.capacityFrames = std::max<uint32_t>(sampleRate / 2, 8192);
  priming.targetFrames = std::min<uint32_t>(
      priming.capacityFrames, std::max<uint32_t>(sampleRate / 4, 4096));
  priming.primeFrames = std::min<uint32_t>(
      priming.targetFrames, std::max<uint32_t>(sampleRate / 10, 4096));
  return priming;
}

bool playbackSourceIsPrimed(uint64_t bufferedFrames,
                            uint32_t primeFrames,
                            bool atEnd) {
  return atEnd || bufferedFrames >= primeFrames;
}
