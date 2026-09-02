#include "playback/session/background_gpu_admission.h"

#include <limits>
#include <utility>

namespace playback_session {

BackgroundGpuAdmissionPolicy::BackgroundGpuAdmissionPolicy(
    BackgroundGpuAdmissionConfig config)
    : config_(std::move(config)) {}

bool BackgroundGpuAdmissionPolicy::queueHasHeadroom(
    const BackgroundGpuSignals& signals, bool alreadyAllowed) const {
  if (!signals.hasVideoFrame) return true;
  const std::int64_t requiredBufferUs =
      (alreadyAllowed ? config_.retainBuffer : config_.grantBuffer).count();
  if (signals.lastPresentedDurationUs > 0 &&
      signals.videoQueueDepth <=
          static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)() /
                                   signals.lastPresentedDurationUs)) {
    return static_cast<std::int64_t>(signals.videoQueueDepth) *
               signals.lastPresentedDurationUs >=
           requiredBufferUs;
  }
  return signals.videoQueueDepth >= (alreadyAllowed
                                         ? config_.retainFallbackFrames
                                         : config_.grantFallbackFrames);
}

BackgroundGpuDecision BackgroundGpuAdmissionPolicy::update(
    const BackgroundGpuSignals& signals, Clock::time_point now) {
  const bool wasAllowed = allowed_;
  lastUpdate_ = now;
  lastForegroundStable_ = false;
  lastQueueHasHeadroom_ = false;

  if (signals.foreground == ForegroundPlaybackActivity::Inactive) {
    allowed_ = true;
    healthySince_ = Clock::time_point::min();
  } else if (signals.foreground != ForegroundPlaybackActivity::Active) {
    allowed_ = false;
    healthySince_ = Clock::time_point::min();
  } else {
    if (previousForeground_ != ForegroundPlaybackActivity::Active) {
      // Resuming playback starts a fresh admission window even if background
      // work was allowed while paused.
      allowed_ = false;
      healthySince_ = now;
    }
    lastForegroundStable_ =
        !signals.seekPending && !signals.buffering && !signals.audioStarved;
    if (!lastForegroundStable_) {
      allowed_ = false;
      healthySince_ = Clock::time_point::min();
    } else {
      if (healthySince_ == Clock::time_point::min()) healthySince_ = now;
      lastQueueHasHeadroom_ = queueHasHeadroom(signals, allowed_);
      if (allowed_ && !lastQueueHasHeadroom_) {
        allowed_ = false;
        healthySince_ = now;
      } else if (!allowed_ && lastQueueHasHeadroom_ &&
                 now - healthySince_ >= config_.stableBeforeGrant) {
        allowed_ = true;
      }
    }
  }
  previousForeground_ = signals.foreground;

  BackgroundGpuDecision decision;
  decision.allowed = allowed_;
  decision.changed = allowed_ != wasAllowed;
  decision.foregroundStable = lastForegroundStable_;
  decision.queueHasHeadroom = lastQueueHasHeadroom_;
  if (healthySince_ != Clock::time_point::min()) {
    decision.healthyFor = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - healthySince_);
  }
  return decision;
}

bool BackgroundGpuAdmissionPolicy::allowed() const { return allowed_; }

std::optional<BackgroundGpuAdmissionPolicy::Clock::time_point>
BackgroundGpuAdmissionPolicy::nextEvaluationDeadline() const {
  if (allowed_ || !lastForegroundStable_ || !lastQueueHasHeadroom_ ||
      healthySince_ == Clock::time_point::min() ||
      lastUpdate_ == Clock::time_point::min()) {
    return std::nullopt;
  }
  const Clock::time_point deadline = healthySince_ + config_.stableBeforeGrant;
  return deadline > lastUpdate_ ? std::optional<Clock::time_point>(deadline)
                                : std::nullopt;
}

void BackgroundGpuAdmissionPolicy::reset() {
  previousForeground_ = ForegroundPlaybackActivity::Unavailable;
  healthySince_ = Clock::time_point::min();
  lastUpdate_ = Clock::time_point::min();
  allowed_ = false;
  lastForegroundStable_ = false;
  lastQueueHasHeadroom_ = false;
}

}  // namespace playback_session
