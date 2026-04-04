#include "playback_sync.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace playback_sync {
namespace {

constexpr int64_t kDefaultFrameDurationUs = 33333;
constexpr int64_t kMaxFrameDurationUs = 500000;
constexpr int64_t kFrameStallThresholdUs = 4000000;
constexpr size_t kDurationHistorySize = 10;
constexpr int64_t kPtsGlitchThresholdUs = 1000000;
constexpr int64_t kSyncLostThresholdUs = 2500000;
constexpr int64_t kSyncWindowUs = 100000;
constexpr int64_t kLowAudioBufferUs = 100000;
constexpr int64_t kRecoveredAudioBufferUs = 200000;

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

int64_t fallbackFrameDuration(int64_t fallbackFrameDurationUs) {
  return fallbackFrameDurationUs > 0 ? fallbackFrameDurationUs
                                     : kDefaultFrameDurationUs;
}

}  // namespace

void initializeLoopState(LoopState& state, int initialSerial,
                         int64_t fallbackFrameDurationUs, int64_t nowUs) {
  state.frameTimerUs = nowUs;
  state.lastPtsUs = -1;
  state.lastDelayUsValue = fallbackFrameDuration(fallbackFrameDurationUs);
  state.lastPresentedTimeUs = nowUs;
  state.lastSerial = initialSerial;
  state.firstPresentedForSerial = false;
  state.lastSanitizedPtsUs = -1;
  state.ptsOffsetUs = 0;
  state.lastSanitizedSerial = 0;
  state.recentDurations.clear();
}

void notePlaybackStateChange(LoopState& state, int64_t nowUs) {
  state.frameTimerUs = nowUs;
}

void syncLoopSerial(LoopState& state, int serial,
                    int64_t fallbackFrameDurationUs, int64_t nowUs) {
  if (state.lastSerial == serial) {
    return;
  }
  state.lastSerial = serial;
  state.firstPresentedForSerial = false;
  state.lastPtsUs = -1;
  state.lastDelayUsValue = fallbackFrameDuration(fallbackFrameDurationUs);
  state.frameTimerUs = nowUs;
  state.lastPresentedTimeUs = nowUs;
  state.lastSanitizedPtsUs = -1;
  state.ptsOffsetUs = 0;
  state.lastSanitizedSerial = 0;
  state.recentDurations.clear();
}

bool shouldBackoffForEmptyQueue(const LoopState& state, PlayerState playbackState,
                                bool seekPending, int64_t durationUs,
                                int64_t currentPresentedPtsUs, int64_t nowUs) {
  if (nowUs - state.lastPresentedTimeUs <= kFrameStallThresholdUs) {
    return false;
  }
  if (playbackState != PlayerState::Playing &&
      playbackState != PlayerState::Draining) {
    return false;
  }
  if (!state.firstPresentedForSerial || seekPending) {
    return false;
  }
  return durationUs <= 0 || currentPresentedPtsUs < durationUs - 1000000;
}

bool isFrameBackwards(const LoopState& state, const QueuedFrame& frame) {
  return state.lastPtsUs >= 0 && frame.ptsUs < state.lastPtsUs;
}

PreparedFrame prepareFrame(LoopState& state, const QueuedFrame& front,
                           const QueuedFrame* next) {
  PreparedFrame prepared;
  prepared.frame = front;

  if (front.serial != state.lastSanitizedSerial) {
    state.lastSanitizedPtsUs = -1;
    state.ptsOffsetUs = 0;
    state.lastSanitizedSerial = front.serial;
  }

  if (state.lastSanitizedPtsUs >= 0) {
    int64_t predictedPtsUs = state.lastSanitizedPtsUs + front.durationUs;
    int64_t adjustedRawPts = front.ptsUs + state.ptsOffsetUs;
    prepared.ptsRepairErrorUs = adjustedRawPts - predictedPtsUs;
    if (std::abs(prepared.ptsRepairErrorUs) > kPtsGlitchThresholdUs) {
      state.ptsOffsetUs -= prepared.ptsRepairErrorUs;
      prepared.frame.ptsUs = predictedPtsUs;
      prepared.discontinuity = true;
      prepared.hadMassiveGlitch = true;
    } else {
      prepared.frame.ptsUs = adjustedRawPts;
    }
  }

  state.lastSanitizedPtsUs = prepared.frame.ptsUs;

  int64_t durationFromPtsUs = 0;
  if (next && next->serial == prepared.frame.serial) {
    int64_t nextPtsUs = next->ptsUs + state.ptsOffsetUs;
    int64_t ptsDiffUs = nextPtsUs - prepared.frame.ptsUs;
    if (ptsDiffUs > 0 && ptsDiffUs <= kMaxFrameDurationUs) {
      durationFromPtsUs = ptsDiffUs;
    }
  }

  int64_t baseDurationUs =
      durationFromPtsUs > 0 ? durationFromPtsUs : prepared.frame.durationUs;
  prepared.delayUs =
      baseDurationUs > 0 ? baseDurationUs : state.lastDelayUsValue;
  prepared.actualDurationUs = prepared.delayUs;

  if (prepared.frame.durationUs > 0 &&
      prepared.frame.durationUs <= kMaxFrameDurationUs) {
    state.recentDurations.push_back(prepared.frame.durationUs);
    if (state.recentDurations.size() > kDurationHistorySize) {
      state.recentDurations.erase(state.recentDurations.begin());
    }
  }

  int64_t smoothedDurationUs = state.lastDelayUsValue;
  if (!state.recentDurations.empty()) {
    auto sorted = state.recentDurations;
    std::sort(sorted.begin(), sorted.end());
    smoothedDurationUs = sorted[sorted.size() / 2];
  }

  if (prepared.delayUs <= 0 || prepared.delayUs > kMaxFrameDurationUs) {
    prepared.delayUs = smoothedDurationUs > 0 ? smoothedDurationUs
                                              : state.lastDelayUsValue;
  } else if (!state.recentDurations.empty()) {
    prepared.delayUs =
        (prepared.delayUs * 40 + smoothedDurationUs * 60) / 100;
  }

  if (prepared.delayUs <= 0) {
    prepared.delayUs = state.lastDelayUsValue;
  }

  prepared.actualDurationUs = prepared.delayUs;
  return prepared;
}

FramePlan planFrame(LoopState& state, PlayerState playbackState,
                    const PreparedFrame& prepared,
                    const playback_clock::Snapshot& master, Clock& videoClock,
                    int64_t nowUs) {
  FramePlan plan;
  plan.delayUs = prepared.delayUs;
  plan.actualDurationUs = prepared.actualDurationUs;

  plan.syncLost =
      master.us != 0 &&
      std::abs(prepared.frame.ptsUs - master.us) > kSyncLostThresholdUs;

  if (playbackState != PlayerState::Priming &&
      master.source == PlayerClockSource::Audio && !master.audioClockReady) {
    plan.waitForAudioClock = true;
    return plan;
  }

  if (playbackState == PlayerState::Priming || plan.syncLost) {
    plan.targetUs = state.frameTimerUs + plan.delayUs;
    return plan;
  }

  if (master.source != PlayerClockSource::None) {
    plan.diffUs = prepared.frame.ptsUs - master.us;

    if (master.audioStarved && master.audioClockReady) {
      plan.waitForAudioRecovery = true;
      return plan;
    }

    uint32_t sampleRate = master.audioSampleRate > 0 ? master.audioSampleRate
                                                     : 48000;
    int64_t audioBufferedUs = static_cast<int64_t>(
        (master.audioBufferedFrames * 1000000ULL) / sampleRate);

    if (audioBufferedUs < kLowAudioBufferUs && audioBufferedUs > 0) {
      double currentSpeed =
          videoClock.speed_q16.load(std::memory_order_relaxed) / 65536.0;
      videoClock.set_speed((std::max)(0.95, currentSpeed * 0.99), nowUs);
    } else if (audioBufferedUs > kRecoveredAudioBufferUs) {
      double currentSpeed =
          videoClock.speed_q16.load(std::memory_order_relaxed) / 65536.0;
      if (currentSpeed < 1.0) {
        videoClock.set_speed((std::min)(1.0, currentSpeed * 1.01), nowUs);
      }
    }

    if (plan.diffUs >= -kSyncWindowUs && plan.diffUs <= kSyncWindowUs) {
      if (!prepared.discontinuity) {
        plan.delayUs =
            computeTargetDelayUs(plan.delayUs, prepared.frame.ptsUs, master.us);
      }
    } else if (plan.diffUs < -kSyncWindowUs) {
      if (master.source == PlayerClockSource::Audio) {
        plan.dropForBehind = true;
      } else {
        plan.delayUs = 0;
      }
    } else if (plan.diffUs > kSyncWindowUs) {
      plan.delayUs =
          (std::min)(static_cast<int64_t>(200000), plan.delayUs + plan.diffUs);
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
  state.lastPtsUs = prepared.frame.ptsUs;
  state.frameTimerUs = targetUs;
  state.lastPresentedTimeUs = nowUs;
}

void notePresentedFrame(LoopState& state, const PreparedFrame& prepared,
                        int64_t delayUs, int64_t targetUs, int64_t nowUs) {
  state.lastPtsUs = prepared.frame.ptsUs;
  state.lastDelayUsValue = delayUs;
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

}  // namespace playback_sync
