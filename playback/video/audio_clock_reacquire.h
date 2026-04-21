#pragma once

#include <atomic>
#include <cstdint>

namespace playback_audio_clock_reacquire {

struct Snapshot {
  bool pending = false;
  bool audioMayDriveMaster = true;
  int serial = 0;
  int64_t targetUs = 0;
};

class Gate {
 public:
  void reset();
  void require(int serial, int64_t targetUs);
  void cancel(int serial);
  Snapshot snapshot(int currentSerial) const;
  void noteQueuedAudio(int serial, int64_t ptsUs, uint64_t writtenFrames);

 private:
  std::atomic<bool> pending_{false};
  std::atomic<int> serial_{0};
  std::atomic<int64_t> targetUs_{0};
};

}  // namespace playback_audio_clock_reacquire
