#pragma once

#include <cstdint>

// One discontinuity applied to the externally queued audio stream. Generation
// identifies its place in the reset protocol; the remaining fields are the
// exact transport state committed by the audio processing thread.
struct AudioStreamReset {
  uint64_t generation = 0;
  int serial = 0;
  int64_t discardUntilUs = 0;
  uint64_t framePosition = 0;
  bool resetPlaybackPosition = false;
};
