#include "serial.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace playback_video_serial_control {
namespace {

constexpr int64_t kSeekPrerollUs = 1000000;

int64_t clampMaximumUs(int64_t maximumUs) {
  return maximumUs > 0 ? maximumUs
                       : (std::numeric_limits<int64_t>::max)();
}

int64_t clampPositionUs(int64_t positionUs, int64_t maximumUs) {
  return std::clamp(positionUs, int64_t{0}, clampMaximumUs(maximumUs));
}

int64_t addClampedPositionUs(int64_t positionUs, int64_t deltaUs,
                             int64_t maximumUs) {
  const int64_t maximum = clampMaximumUs(maximumUs);
  const int64_t base = clampPositionUs(positionUs, maximum);
  if (deltaUs >= 0) {
    if (deltaUs >= maximum || base >= maximum - deltaUs) {
      return maximum;
    }
    return base + deltaUs;
  }

  const uint64_t magnitude =
      static_cast<uint64_t>(-(deltaUs + 1)) + uint64_t{1};
  if (magnitude >= static_cast<uint64_t>(base)) {
    return 0;
  }
  return base - static_cast<int64_t>(magnitude);
}

}  // namespace

void Controller::reset() {
  std::lock_guard<std::mutex> lock(transitionMutex_);
  seekDisplayUs_.store(0, std::memory_order_relaxed);
  seekInFlightSerial_.store(0, std::memory_order_relaxed);
  seekFailed_.store(false, std::memory_order_relaxed);
  seekPending_.store(false, std::memory_order_relaxed);
  seekTargetUs_.store(0, std::memory_order_relaxed);
  sourceTargetUs_.store(0, std::memory_order_relaxed);
  demuxWindowEndUs_.store(0, std::memory_order_relaxed);
  presentationTargetUs_.store(0, std::memory_order_relaxed);
  decoderPrerollTargetUs_.store(0, std::memory_order_relaxed);
  demuxSeekMode_.store(static_cast<int>(DemuxSeekMode::Timeline),
                       std::memory_order_relaxed);
  pendingSeekSerial_.store(0, std::memory_order_relaxed);
  presentationTargetSerial_.store(0, std::memory_order_relaxed);
  decoderPrerollTargetSerial_.store(0, std::memory_order_relaxed);
  latestSeekRequestGeneration_ = 0;
  handledSeekRequestGeneration_ = 0;
  requestedSeekUs_ = 0;
  presentedPositionValid_ = false;
  presentedPositionSerial_ = 0;
  presentedPositionUs_ = 0;
  currentSerial_.store(1, std::memory_order_release);
}

void Controller::startSession(int initialSerial) {
  std::lock_guard<std::mutex> lock(transitionMutex_);
  seekDisplayUs_.store(0, std::memory_order_relaxed);
  seekInFlightSerial_.store(0, std::memory_order_relaxed);
  seekFailed_.store(false, std::memory_order_relaxed);
  seekPending_.store(false, std::memory_order_relaxed);
  seekTargetUs_.store(0, std::memory_order_relaxed);
  sourceTargetUs_.store(0, std::memory_order_relaxed);
  demuxWindowEndUs_.store(0, std::memory_order_relaxed);
  presentationTargetUs_.store(0, std::memory_order_relaxed);
  decoderPrerollTargetUs_.store(0, std::memory_order_relaxed);
  demuxSeekMode_.store(static_cast<int>(DemuxSeekMode::Timeline),
                       std::memory_order_relaxed);
  pendingSeekSerial_.store(0, std::memory_order_relaxed);
  presentationTargetSerial_.store(0, std::memory_order_relaxed);
  decoderPrerollTargetSerial_.store(0, std::memory_order_relaxed);
  latestSeekRequestGeneration_ = 0;
  handledSeekRequestGeneration_ = 0;
  requestedSeekUs_ = 0;
  presentedPositionValid_ = false;
  presentedPositionSerial_ = 0;
  presentedPositionUs_ = 0;
  currentSerial_.store(initialSerial, std::memory_order_release);
}

SeekRequest Controller::publishSeekRequest(int64_t targetUs,
                                           int64_t maximumUs) {
  std::lock_guard<std::mutex> lock(transitionMutex_);
  SeekRequest request;
  request.generation = ++latestSeekRequestGeneration_;
  request.targetUs = clampPositionUs(targetUs, maximumUs);
  requestedSeekUs_ = request.targetUs;
  return request;
}

SeekRequest Controller::publishRelativeSeekRequest(int64_t deltaUs,
                                                   int64_t maximumUs) {
  std::lock_guard<std::mutex> lock(transitionMutex_);
  const bool requestPending =
      latestSeekRequestGeneration_ != handledSeekRequestGeneration_;
  const bool transitionPending =
      seekPending_.load(std::memory_order_relaxed) ||
      seekInFlightSerial_.load(std::memory_order_relaxed) != 0 ||
      pendingSeekSerial_.load(std::memory_order_relaxed) != 0;

  int64_t baseUs = 0;
  if (requestPending) {
    baseUs = requestedSeekUs_;
  } else if (transitionPending) {
    baseUs = seekDisplayUs_.load(std::memory_order_relaxed);
  } else if (presentedPositionValid_ &&
             presentedPositionSerial_ ==
                 currentSerial_.load(std::memory_order_relaxed)) {
    baseUs = presentedPositionUs_;
  }

  SeekRequest request;
  request.generation = ++latestSeekRequestGeneration_;
  request.targetUs = addClampedPositionUs(baseUs, deltaUs, maximumUs);
  requestedSeekUs_ = request.targetUs;
  return request;
}

void Controller::acknowledgeSeekRequest(uint64_t generation) {
  std::lock_guard<std::mutex> lock(transitionMutex_);
  if (generation > handledSeekRequestGeneration_ &&
      generation <= latestSeekRequestGeneration_) {
    handledSeekRequestGeneration_ = generation;
  }
}

void Controller::notePresentedPosition(int serial, int64_t ptsUs) {
  if (serial <= 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(transitionMutex_);
  if (serial != currentSerial_.load(std::memory_order_relaxed)) {
    return;
  }
  presentedPositionValid_ = true;
  presentedPositionSerial_ = serial;
  presentedPositionUs_ = (std::max)(int64_t{0}, ptsUs);
}

TransitionPlan Controller::beginTransition(int64_t targetUs, bool initDone,
                                           bool running) {
  int64_t clampedTargetUs = (std::max)(int64_t{0}, targetUs);
  return beginTransition(
      clampedTargetUs, (std::max)(int64_t{0}, clampedTargetUs - kSeekPrerollUs),
      initDone, running);
}

TransitionPlan Controller::beginTransition(int64_t displayTargetUs,
                                           int64_t demuxTargetUs,
                                           bool initDone, bool running) {
  return beginTransition(displayTargetUs, demuxTargetUs, displayTargetUs,
                         displayTargetUs, DemuxSeekMode::Timeline,
                         initDone, running);
}

TransitionPlan Controller::beginTransition(int64_t displayTargetUs,
                                           int64_t demuxTargetUs,
                                           int64_t decoderPrerollTargetUs,
                                           bool initDone, bool running) {
  return beginTransition(displayTargetUs, demuxTargetUs, displayTargetUs,
                         decoderPrerollTargetUs, DemuxSeekMode::Timeline,
                         initDone, running);
}

TransitionPlan Controller::beginTransition(int64_t displayTargetUs,
                                           int64_t demuxTargetUs,
                                           int64_t demuxWindowEndUs,
                                           int64_t decoderPrerollTargetUs,
                                           bool initDone, bool running) {
  return beginTransition(displayTargetUs, demuxTargetUs, demuxWindowEndUs,
                         decoderPrerollTargetUs, DemuxSeekMode::Timeline,
                         initDone, running);
}

TransitionPlan Controller::beginTransition(int64_t displayTargetUs,
                                           int64_t demuxTargetUs,
                                           int64_t demuxWindowEndUs,
                                           int64_t decoderPrerollTargetUs,
                                           DemuxSeekMode demuxSeekMode,
                                           bool initDone, bool running) {
  return beginTransition(displayTargetUs, displayTargetUs, demuxTargetUs,
                         demuxWindowEndUs, decoderPrerollTargetUs,
                         demuxSeekMode, initDone, running);
}

TransitionPlan Controller::beginTransition(int64_t displayTargetUs,
                                           int64_t sourceTargetUs,
                                           int64_t demuxTargetUs,
                                           int64_t demuxWindowEndUs,
                                           int64_t decoderPrerollTargetUs,
                                           DemuxSeekMode demuxSeekMode,
                                           bool initDone, bool running) {
  TransitionPlan plan;
  if (!running) {
    return plan;
  }

  std::lock_guard<std::mutex> lock(transitionMutex_);

  int nextSerial = currentSerial_.load(std::memory_order_relaxed) + 1;
  int64_t clampedDisplayTargetUs = (std::max)(int64_t{0}, displayTargetUs);
  int64_t clampedSourceTargetUs = (std::max)(int64_t{0}, sourceTargetUs);
  int64_t clampedDemuxTargetUs = (std::max)(int64_t{0}, demuxTargetUs);
  int64_t clampedDemuxWindowEndUs =
      (std::max)(int64_t{0}, demuxWindowEndUs);
  int64_t clampedDecoderPrerollTargetUs =
      (std::max)(int64_t{0}, decoderPrerollTargetUs);
  assert(clampedDemuxTargetUs <= clampedSourceTargetUs);
  assert(clampedDemuxTargetUs <= clampedDemuxWindowEndUs);
  assert(clampedDemuxWindowEndUs <= clampedSourceTargetUs);
  assert(clampedDecoderPrerollTargetUs <= clampedSourceTargetUs);

  seekInFlightSerial_.store(nextSerial, std::memory_order_relaxed);
  seekFailed_.store(false, std::memory_order_relaxed);
  pendingSeekSerial_.store(nextSerial, std::memory_order_relaxed);
  presentationTargetSerial_.store(nextSerial, std::memory_order_relaxed);
  decoderPrerollTargetSerial_.store(nextSerial, std::memory_order_relaxed);
  seekDisplayUs_.store(clampedDisplayTargetUs, std::memory_order_relaxed);
  seekTargetUs_.store(clampedDemuxTargetUs, std::memory_order_relaxed);
  sourceTargetUs_.store(clampedSourceTargetUs, std::memory_order_relaxed);
  demuxWindowEndUs_.store(clampedDemuxWindowEndUs,
                          std::memory_order_relaxed);
  presentationTargetUs_.store(clampedDisplayTargetUs, std::memory_order_relaxed);
  decoderPrerollTargetUs_.store(clampedDecoderPrerollTargetUs,
                                std::memory_order_relaxed);
  demuxSeekMode_.store(static_cast<int>(demuxSeekMode),
                       std::memory_order_relaxed);
  seekPending_.store(true, std::memory_order_relaxed);
  currentSerial_.store(nextSerial, std::memory_order_release);

  plan.valid = true;
  plan.serial = nextSerial;
  plan.displayTargetUs = clampedDisplayTargetUs;
  plan.sourceTargetUs = clampedSourceTargetUs;
  plan.demuxTargetUs = clampedDemuxTargetUs;
  plan.demuxWindowEndUs = clampedDemuxWindowEndUs;
  plan.decoderPrerollTargetUs = clampedDecoderPrerollTargetUs;
  plan.demuxSeekMode = demuxSeekMode;
  plan.signalCommandPending = initDone;
  return plan;
}

PendingSeek Controller::claimPendingSeek() {
  std::lock_guard<std::mutex> lock(transitionMutex_);
  PendingSeek pending;
  if (!seekPending_.exchange(false, std::memory_order_relaxed)) {
    return pending;
  }
  pending.valid = true;
  pending.serial = currentSerial_.load(std::memory_order_relaxed);
  pending.demuxTargetUs = seekTargetUs_.load(std::memory_order_relaxed);
  pending.sourceTargetUs = sourceTargetUs_.load(std::memory_order_relaxed);
  pending.demuxWindowEndUs =
      demuxWindowEndUs_.load(std::memory_order_relaxed);
  pending.displayTargetUs = seekDisplayUs_.load(std::memory_order_relaxed);
  pending.decoderPrerollTargetUs =
      decoderPrerollTargetUs_.load(std::memory_order_relaxed);
  pending.demuxSeekMode = static_cast<DemuxSeekMode>(
      demuxSeekMode_.load(std::memory_order_relaxed));
  return pending;
}

bool Controller::applySeekResult(int serial, int resultCode) {
  std::lock_guard<std::mutex> lock(transitionMutex_);
  if (serial != currentSerial_.load(std::memory_order_relaxed)) {
    return false;
  }
  seekFailed_.store(resultCode != 0, std::memory_order_relaxed);
  seekInFlightSerial_.store(0, std::memory_order_relaxed);
  if (resultCode != 0) {
    seekDisplayUs_.store(0, std::memory_order_relaxed);
    sourceTargetUs_.store(0, std::memory_order_relaxed);
    pendingSeekSerial_.store(0, std::memory_order_relaxed);
    demuxWindowEndUs_.store(0, std::memory_order_relaxed);
    presentationTargetUs_.store(0, std::memory_order_relaxed);
    presentationTargetSerial_.store(0, std::memory_order_relaxed);
    decoderPrerollTargetUs_.store(0, std::memory_order_relaxed);
    decoderPrerollTargetSerial_.store(0, std::memory_order_relaxed);
    demuxSeekMode_.store(static_cast<int>(DemuxSeekMode::Timeline),
                         std::memory_order_relaxed);
  }
  return true;
}

void Controller::clearSeekFailure() {
  std::lock_guard<std::mutex> lock(transitionMutex_);
  seekFailed_.store(false, std::memory_order_relaxed);
}

bool Controller::clearPendingPresentation(int serial) {
  std::lock_guard<std::mutex> lock(transitionMutex_);
  int expected = serial;
  if (!pendingSeekSerial_.compare_exchange_strong(
          expected, 0, std::memory_order_relaxed)) {
    return false;
  }
  seekDisplayUs_.store(0, std::memory_order_relaxed);
  return true;
}

int Controller::currentSerial() const {
  return currentSerial_.load(std::memory_order_acquire);
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

PositionSnapshot Controller::positionSnapshot() const {
  std::lock_guard<std::mutex> lock(transitionMutex_);
  PositionSnapshot snapshot;
  snapshot.currentSerial = currentSerial_.load(std::memory_order_relaxed);
  snapshot.latestSeekRequestGeneration = latestSeekRequestGeneration_;
  snapshot.handledSeekRequestGeneration = handledSeekRequestGeneration_;
  snapshot.requestPending =
      latestSeekRequestGeneration_ != handledSeekRequestGeneration_;
  snapshot.seekPending = seekPending_.load(std::memory_order_relaxed);
  snapshot.pendingSeekSerial =
      pendingSeekSerial_.load(std::memory_order_relaxed);
  snapshot.seekInFlightSerial =
      seekInFlightSerial_.load(std::memory_order_relaxed);
  snapshot.seekDisplayUs = seekDisplayUs_.load(std::memory_order_relaxed);
  snapshot.requestedSeekUs = requestedSeekUs_;
  snapshot.presentedPositionValid = presentedPositionValid_;
  snapshot.presentedPositionSerial = presentedPositionSerial_;
  snapshot.presentedPositionUs = presentedPositionUs_;
  snapshot.transitionPending =
      snapshot.seekPending || snapshot.seekInFlightSerial != 0 ||
      snapshot.pendingSeekSerial != 0;
  if (snapshot.requestPending) {
    snapshot.positionUs = snapshot.requestedSeekUs;
  } else if (snapshot.transitionPending) {
    snapshot.positionUs = snapshot.seekDisplayUs;
  } else if (snapshot.presentedPositionValid &&
             snapshot.presentedPositionSerial == snapshot.currentSerial) {
    snapshot.positionUs = snapshot.presentedPositionUs;
  }
  return snapshot;
}

int64_t Controller::presentationTargetUsForSerial(int serial) const {
  if (serial <= 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(transitionMutex_);
  if (presentationTargetSerial_.load(std::memory_order_relaxed) != serial) {
    return 0;
  }
  return presentationTargetUs_.load(std::memory_order_relaxed);
}

int64_t Controller::decoderPrerollTargetUsForSerial(int serial) const {
  if (serial <= 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(transitionMutex_);
  if (decoderPrerollTargetSerial_.load(std::memory_order_relaxed) != serial) {
    return 0;
  }
  return decoderPrerollTargetUs_.load(std::memory_order_relaxed);
}

}  // namespace playback_video_serial_control
