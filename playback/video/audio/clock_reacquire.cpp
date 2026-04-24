#include "clock_reacquire.h"

#include <algorithm>

namespace playback_audio_clock_reacquire {

void Gate::reset() {
  pending_.store(false, std::memory_order_relaxed);
  serial_.store(0, std::memory_order_relaxed);
  targetUs_.store(0, std::memory_order_relaxed);
}

void Gate::require(int serial, int64_t targetUs) {
  if (serial <= 0) {
    reset();
    return;
  }
  targetUs_.store((std::max)(int64_t{0}, targetUs),
                  std::memory_order_relaxed);
  serial_.store(serial, std::memory_order_relaxed);
  pending_.store(true, std::memory_order_release);
}

void Gate::cancel(int serial) {
  if (serial_.load(std::memory_order_relaxed) == serial) {
    pending_.store(false, std::memory_order_release);
  }
}

Snapshot Gate::snapshot(int currentSerial) const {
  Snapshot snapshot;
  snapshot.pending =
      pending_.load(std::memory_order_acquire) &&
      serial_.load(std::memory_order_relaxed) == currentSerial;
  snapshot.serial = serial_.load(std::memory_order_relaxed);
  snapshot.targetUs = targetUs_.load(std::memory_order_relaxed);
  snapshot.audioMayDriveMaster = !snapshot.pending;
  return snapshot;
}

void Gate::noteQueuedAudio(int serial, int64_t ptsUs, uint64_t writtenFrames) {
  if (writtenFrames == 0 || ptsUs < 0) {
    return;
  }
  if (!pending_.load(std::memory_order_acquire)) {
    return;
  }
  if (serial_.load(std::memory_order_relaxed) != serial) {
    return;
  }
  if (ptsUs >= targetUs_.load(std::memory_order_relaxed)) {
    pending_.store(false, std::memory_order_release);
  }
}

}  // namespace playback_audio_clock_reacquire
