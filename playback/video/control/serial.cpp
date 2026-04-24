#include "serial.h"

#include <algorithm>

namespace playback_video_serial_control {
namespace {

constexpr int64_t kSeekPrerollUs = 1000000;

}  // namespace

void Controller::reset() {
  seekDisplayUs_.store(0, std::memory_order_relaxed);
  seekInFlightSerial_.store(0, std::memory_order_relaxed);
  seekFailed_.store(false, std::memory_order_relaxed);
  seekPending_.store(false, std::memory_order_relaxed);
  seekTargetUs_.store(0, std::memory_order_relaxed);
  presentationTargetUs_.store(0, std::memory_order_relaxed);
  currentSerial_.store(1, std::memory_order_relaxed);
  pendingSeekSerial_.store(0, std::memory_order_relaxed);
  presentationTargetSerial_.store(0, std::memory_order_relaxed);
}

void Controller::startSession(int initialSerial) {
  currentSerial_.store(initialSerial, std::memory_order_relaxed);
  seekDisplayUs_.store(0, std::memory_order_relaxed);
  seekInFlightSerial_.store(0, std::memory_order_relaxed);
  seekFailed_.store(false, std::memory_order_relaxed);
  seekPending_.store(false, std::memory_order_relaxed);
  seekTargetUs_.store(0, std::memory_order_relaxed);
  presentationTargetUs_.store(0, std::memory_order_relaxed);
  pendingSeekSerial_.store(0, std::memory_order_relaxed);
  presentationTargetSerial_.store(0, std::memory_order_relaxed);
}

TransitionPlan Controller::beginTransition(int64_t targetUs, bool initDone,
                                           bool running) {
  TransitionPlan plan;
  if (!running) {
    return plan;
  }

  int nextSerial = currentSerial_.load(std::memory_order_relaxed) + 1;
  int64_t clampedTargetUs = (std::max)(int64_t{0}, targetUs);
  int64_t demuxTargetUs =
      (std::max)(int64_t{0}, clampedTargetUs - kSeekPrerollUs);

  currentSerial_.store(nextSerial, std::memory_order_relaxed);
  seekInFlightSerial_.store(nextSerial, std::memory_order_relaxed);
  seekFailed_.store(false, std::memory_order_relaxed);
  pendingSeekSerial_.store(nextSerial, std::memory_order_relaxed);
  presentationTargetSerial_.store(nextSerial, std::memory_order_relaxed);
  seekDisplayUs_.store(clampedTargetUs, std::memory_order_relaxed);
  seekTargetUs_.store(demuxTargetUs, std::memory_order_relaxed);
  presentationTargetUs_.store(clampedTargetUs, std::memory_order_relaxed);
  seekPending_.store(true, std::memory_order_relaxed);

  plan.valid = true;
  plan.serial = nextSerial;
  plan.displayTargetUs = clampedTargetUs;
  plan.demuxTargetUs = demuxTargetUs;
  plan.signalCommandPending = initDone;
  return plan;
}

PendingSeek Controller::claimPendingSeek() {
  PendingSeek pending;
  if (!seekPending_.exchange(false, std::memory_order_relaxed)) {
    return pending;
  }
  pending.valid = true;
  pending.serial = currentSerial_.load(std::memory_order_relaxed);
  pending.demuxTargetUs = seekTargetUs_.load(std::memory_order_relaxed);
  pending.displayTargetUs = seekDisplayUs_.load(std::memory_order_relaxed);
  return pending;
}

bool Controller::applySeekResult(int serial, int resultCode) {
  if (serial != currentSerial_.load(std::memory_order_relaxed)) {
    return false;
  }
  seekFailed_.store(resultCode != 0, std::memory_order_relaxed);
  seekInFlightSerial_.store(0, std::memory_order_relaxed);
  if (resultCode != 0) {
    seekDisplayUs_.store(0, std::memory_order_relaxed);
    pendingSeekSerial_.store(0, std::memory_order_relaxed);
    presentationTargetUs_.store(0, std::memory_order_relaxed);
    presentationTargetSerial_.store(0, std::memory_order_relaxed);
  }
  return true;
}

void Controller::clearSeekFailure() {
  seekFailed_.store(false, std::memory_order_relaxed);
}

void Controller::clearPendingPresentation(int serial) {
  if (pendingSeekSerial_.load(std::memory_order_relaxed) != serial) {
    return;
  }
  pendingSeekSerial_.store(0, std::memory_order_relaxed);
  seekDisplayUs_.store(0, std::memory_order_relaxed);
}

int Controller::currentSerial() const {
  return currentSerial_.load(std::memory_order_relaxed);
}

std::atomic<int>* Controller::currentSerialAtomic() { return &currentSerial_; }

bool Controller::seekPending() const {
  return seekPending_.load(std::memory_order_relaxed);
}

int Controller::pendingSeekSerial() const {
  return pendingSeekSerial_.load(std::memory_order_relaxed);
}

int Controller::seekInFlightSerial() const {
  return seekInFlightSerial_.load(std::memory_order_relaxed);
}

bool Controller::seekFailed() const {
  return seekFailed_.load(std::memory_order_relaxed);
}

int64_t Controller::seekDisplayUs() const {
  return seekDisplayUs_.load(std::memory_order_relaxed);
}

int64_t Controller::presentationTargetUsForSerial(int serial) const {
  if (serial <= 0) {
    return 0;
  }
  if (presentationTargetSerial_.load(std::memory_order_relaxed) != serial) {
    return 0;
  }
  return presentationTargetUs_.load(std::memory_order_relaxed);
}

}  // namespace playback_video_serial_control
