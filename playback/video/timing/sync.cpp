#include "sync.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

#include "playback/video/state/machine.h"

namespace playback_video_sync {
namespace {

constexpr int64_t kDefaultFrameDurationUs = 33333;
constexpr int64_t kMaxFrameDurationUs = 500000;
constexpr int64_t kFrameStallThresholdUs = 4000000;
constexpr size_t kDurationHistorySize = 10;
constexpr int64_t kDiscontinuityThresholdUs = 1000000;
constexpr int64_t kSyncLostThresholdUs = 2500000;
constexpr int64_t kSyncWindowUs = 100000;

int64_t clampi64(int64_t v, int64_t lo, int64_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

int64_t computeTargetDelayUs(int64_t delayUs, int64_t videoPtsUs,
                             int64_t masterUs) {
  const int64_t kSyncThresholdMin = 10000;
  const int64_t kSyncThresholdMax = 100000;

  int64_t diff = videoPtsUs - masterUs;
  int64_t syncThreshold =
      clampi64(delayUs, kSyncThresholdMin, kSyncThresholdMax);

  if (std::abs(diff) < 10000000) {
    if (diff <= -syncThreshold) {
      delayUs = std::max<int64_t>(0, delayUs + diff);
    } else if (diff >= syncThreshold) {
      if (diff > 500000) {
        delayUs = 500000;
      } else {
        delayUs = delayUs + diff;
      }
    }
  }

  return delayUs;
}

int64_t frameDurationEstimateOrDefault(int64_t estimatedFrameDurationUs) {
  return estimatedFrameDurationUs > 0 ? estimatedFrameDurationUs
                                      : kDefaultFrameDurationUs;
}

int64_t measuredFrameDurationUs(const QueuedFrame& frame,
                                const QueuedFrame* next, bool* measured) {
  if (measured) {
    *measured = false;
  }
  if (next && next->serial == frame.serial) {
    assert(next->displayIndex > frame.displayIndex);
    const int64_t ptsDiffUs = next->ptsUs - frame.ptsUs;
    if (ptsDiffUs > 0 && ptsDiffUs <= kMaxFrameDurationUs) {
      if (measured) {
        *measured = true;
      }
      return ptsDiffUs;
    }
  }
  if (frame.durationUs > 0 && frame.durationUs <= kMaxFrameDurationUs) {
    if (measured) {
      *measured = true;
    }
    return frame.durationUs;
  }
  return 0;
}

}  // namespace

void initializeLoopState(LoopState& state, int initialSerial,
                         int64_t estimatedFrameDurationUs, int64_t nowUs) {
  state.frameTimerUs = nowUs;
  state.lastPtsUs = -1;
  state.lastFrameDurationUs =
      frameDurationEstimateOrDefault(estimatedFrameDurationUs);
  state.lastPresentedTimeUs = nowUs;
  state.lastDisplayIndex = 0;
  state.lastSerial = initialSerial;
  state.firstPresentedForSerial = false;
  state.recentDurations.clear();
}

void notePlaybackStateChange(LoopState& state, int64_t nowUs) {
  state.frameTimerUs = nowUs;
}

bool syncLoopSerial(LoopState& state, int serial,
                    int64_t estimatedFrameDurationUs, int64_t nowUs) {
  if (state.lastSerial == serial) {
    return false;
  }
  state.lastSerial = serial;
  state.firstPresentedForSerial = false;
  state.lastPtsUs = -1;
  state.lastDisplayIndex = 0;
  state.lastFrameDurationUs =
      frameDurationEstimateOrDefault(estimatedFrameDurationUs);
  state.frameTimerUs = nowUs;
  state.lastPresentedTimeUs = nowUs;
  state.recentDurations.clear();
  return true;
}

bool shouldBackoffForEmptyQueue(const LoopState& state, PlayerState playbackState,
                                bool seekPending, int64_t durationUs,
                                int64_t currentPresentedPtsUs, int64_t nowUs) {
  if (nowUs - state.lastPresentedTimeUs <= kFrameStallThresholdUs) {
    return false;
  }
  playback_video_state_machine::StateProjection projection =
      playback_video_state_machine::project(playbackState);
  if (projection.transport != playback_video_state_machine::TransportState::Playing ||
      (projection.buffering != playback_video_state_machine::BufferingState::Ready &&
       projection.buffering !=
           playback_video_state_machine::BufferingState::Draining)) {
    return false;
  }
  if (!state.firstPresentedForSerial || seekPending) {
    return false;
  }
  return durationUs <= 0 || currentPresentedPtsUs < durationUs - 1000000;
}

bool isFrameBackwards(const LoopState& state, const QueuedFrame& frame) {
  if (state.lastDisplayIndex == 0) {
    return false;
  }
  assert(frame.displayIndex > 0);
  return frame.displayIndex <= state.lastDisplayIndex;
}

PreparedFrame prepareFrame(LoopState& state, const QueuedFrame& front,
                           const QueuedFrame* next) {
  assert(front.displayIndex > 0);
  PreparedFrame prepared;
  prepared.frame = front;
  if (state.lastPtsUs >= 0) {
    int64_t expectedPtsUs = state.lastPtsUs + state.lastFrameDurationUs;
    if (std::abs(front.ptsUs - expectedPtsUs) > kDiscontinuityThresholdUs) {
      prepared.discontinuity = true;
    }
  }

  bool measuredDuration = false;
  prepared.frameDurationUs =
      measuredFrameDurationUs(prepared.frame, next, &measuredDuration);
  if (prepared.frameDurationUs <= 0) {
    prepared.frameDurationUs = state.lastFrameDurationUs;
  }
  prepared.delayUs = prepared.frameDurationUs;

  if (measuredDuration) {
    state.recentDurations.push_back(prepared.frameDurationUs);
    if (state.recentDurations.size() > kDurationHistorySize) {
      state.recentDurations.erase(state.recentDurations.begin());
    }
  }

  int64_t smoothedDurationUs = state.lastFrameDurationUs;
  if (!state.recentDurations.empty()) {
    auto sorted = state.recentDurations;
    std::sort(sorted.begin(), sorted.end());
    smoothedDurationUs = sorted[sorted.size() / 2];
  }

  if (prepared.delayUs <= 0 || prepared.delayUs > kMaxFrameDurationUs) {
    prepared.delayUs = smoothedDurationUs > 0 ? smoothedDurationUs
                                              : state.lastFrameDurationUs;
  } else if (!state.recentDurations.empty()) {
    prepared.delayUs =
        (prepared.delayUs * 40 + smoothedDurationUs * 60) / 100;
  }

  if (prepared.delayUs <= 0) {
    prepared.delayUs = state.lastFrameDurationUs;
  }

  return prepared;
}

FramePlan planFrame(LoopState& state, PlayerState playbackState,
                    const PreparedFrame& prepared,
                    const playback_video_main_clock::Snapshot& master, int64_t nowUs) {
  FramePlan plan;
  plan.delayUs = prepared.delayUs;

  plan.syncLost =
      master.us != 0 &&
      std::abs(prepared.frame.ptsUs - master.us) > kSyncLostThresholdUs;

  playback_video_state_machine::StateProjection projection =
      playback_video_state_machine::project(playbackState);
  const bool priming =
      projection.buffering == playback_video_state_machine::BufferingState::Priming;
  const bool seeking =
      projection.transport == playback_video_state_machine::TransportState::Seeking;

  if (!priming && !seeking &&
      master.source == PlayerClockSource::Audio && !master.audioClockReady) {
    plan.waitForAudioClock = true;
    return plan;
  }

  if (seeking) {
    plan.delayUs = 0;
    plan.targetUs = nowUs;
    return plan;
  }

  if (priming || plan.syncLost) {
    plan.targetUs = state.frameTimerUs + plan.delayUs;
    return plan;
  }

  if (master.source != PlayerClockSource::None) {
    plan.diffUs = prepared.frame.ptsUs - master.us;

    if (!master.audioMasterBlocked && master.audioStarved &&
        master.audioClockReady) {
      plan.waitForAudioRecovery = true;
      return plan;
    }

    const int64_t fallbackTargetUs = state.frameTimerUs + plan.delayUs;
    const int64_t convertedTargetUs = playback_video_main_clock::convertToSystemUs(
        master, prepared.frame.ptsUs, fallbackTargetUs);

    if (plan.diffUs >= -kSyncWindowUs && plan.diffUs <= kSyncWindowUs) {
      if (!prepared.discontinuity) {
        plan.delayUs =
            computeTargetDelayUs(plan.delayUs, prepared.frame.ptsUs, master.us);
      }
      plan.targetUs = convertedTargetUs;
      return plan;
    } else if (plan.diffUs < -kSyncWindowUs) {
      if (master.source == PlayerClockSource::Audio) {
        plan.dropForBehind = true;
      } else {
        plan.delayUs = 0;
      }
      plan.targetUs = nowUs;
      return plan;
    } else if (plan.diffUs > kSyncWindowUs) {
      plan.targetUs =
          (std::min)(convertedTargetUs, nowUs + static_cast<int64_t>(200000));
      return plan;
    }
  }

  plan.targetUs = state.frameTimerUs + plan.delayUs;
  return plan;
}

bool shouldDropLateFrame(int64_t nowUs, int64_t targetUs, size_t queueDepth) {
  return nowUs > targetUs + 100000 && queueDepth > 1;
}

void noteLateDrop(LoopState& state, const PreparedFrame& prepared,
                  int64_t targetUs, int64_t nowUs) {
  assert(prepared.frame.displayIndex > 0);
  state.lastPtsUs = prepared.frame.ptsUs;
  state.lastDisplayIndex = prepared.frame.displayIndex;
  state.frameTimerUs = targetUs;
  state.lastPresentedTimeUs = nowUs;
}

void notePresentedFrame(LoopState& state, const PreparedFrame& prepared,
                        int64_t targetUs, int64_t nowUs) {
  assert(prepared.frame.displayIndex > 0);
  state.lastPtsUs = prepared.frame.ptsUs;
  state.lastDisplayIndex = prepared.frame.displayIndex;
  if (prepared.frameDurationUs > 0) {
    state.lastFrameDurationUs = prepared.frameDurationUs;
  }
  state.lastPresentedTimeUs = nowUs;
  state.frameTimerUs = std::max(targetUs, nowUs);
}

PrimingAnchor choosePrimingAnchor(int64_t audioOldestPtsUs, int64_t videoPtsUs) {
  PrimingAnchor anchor;
  if (audioOldestPtsUs >= 0) {
    anchor.valid = true;
    anchor.ptsUs = audioOldestPtsUs;
    return anchor;
  }
  if (videoPtsUs >= 0) {
    anchor.valid = true;
    anchor.ptsUs = videoPtsUs;
  }
  return anchor;
}

}  // namespace playback_video_sync
