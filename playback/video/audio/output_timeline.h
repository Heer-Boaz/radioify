#pragma once

#include <cstdint>

#include "clock_reacquire.h"

namespace playback_audio_output_timeline {

struct ResetResult {
  bool applied = false;
  bool reacquireStarted = false;
  int serial = 0;
  int64_t targetUs = 0;
};

class Controller {
 public:
  void reset();
  ResetResult resetForSerial(int serial, int64_t targetUs,
                             bool reacquireClock);
  void cancelClockReacquire(int serial);
  playback_audio_clock_reacquire::Snapshot clockGateSnapshot(
      int currentSerial) const;
  void noteQueuedAudio(int serial, int64_t ptsUs, uint64_t writtenFrames);

 private:
  playback_audio_clock_reacquire::Gate clockReacquire_;
};

}  // namespace playback_audio_output_timeline
