#pragma once

#include <cstddef>
#include <cstdint>

#include "clock.h"
#include "player.h"

namespace playback_clock {

struct Snapshot {
  int64_t us = 0;
  PlayerClockSource source = PlayerClockSource::None;
  bool audioClockReady = false;
  bool audioClockFresh = false;
  bool audioStarved = false;
  int64_t audioClockUpdatedUs = 0;
  size_t audioBufferedFrames = 0;
  uint32_t audioSampleRate = 0;
};

Snapshot sample(bool audioActive, int currentSerial, const Clock& videoClock,
                int64_t nowUs);

int64_t resolveCurrentPlaybackUs(const Snapshot& snapshot,
                                 const Clock& videoClock, int currentSerial,
                                 int64_t lastPresentedPtsUs, int64_t nowUs);

void synchronizePrimingClocks(int serial, int64_t ptsUs, Clock& videoClock,
                              int64_t nowUs);

void synchronizeAudioClockOnly(int serial, int64_t ptsUs);

}  // namespace playback_clock
