#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace playback_session {

enum class ForegroundPlaybackActivity : std::uint8_t {
  Unavailable,
  Active,
  Inactive,
};

struct BackgroundGpuSignals {
  ForegroundPlaybackActivity foreground =
      ForegroundPlaybackActivity::Unavailable;
  bool seekPending = false;
  bool buffering = false;
  bool audioStarved = false;
  bool hasVideoFrame = false;
  std::size_t videoQueueDepth = 0;
  std::int64_t lastPresentedDurationUs = 0;
};

struct BackgroundGpuAdmissionConfig {
  std::chrono::microseconds grantBuffer{80'000};
  std::chrono::microseconds retainBuffer{45'000};
  std::size_t grantFallbackFrames = 4;
  std::size_t retainFallbackFrames = 2;
  std::chrono::milliseconds stableBeforeGrant{1'500};
};

struct BackgroundGpuDecision {
  bool allowed = false;
  bool changed = false;
  bool foregroundStable = false;
  bool queueHasHeadroom = false;
  std::optional<std::chrono::milliseconds> healthyFor;
};

// Stateful hysteresis at the foreground-playback ownership boundary. The
// background worker receives GPU time only after playback has remained stable
// with a high queue watermark. Once admitted it retains the lease down to a
// lower watermark, and any foreground pressure revokes it immediately.
class BackgroundGpuAdmissionPolicy {
 public:
  using Clock = std::chrono::steady_clock;

  explicit BackgroundGpuAdmissionPolicy(
      BackgroundGpuAdmissionConfig config = {});

  BackgroundGpuDecision update(const BackgroundGpuSignals& signals,
                               Clock::time_point now);
  bool allowed() const;
  std::optional<Clock::time_point> nextEvaluationDeadline() const;
  void reset();

 private:
  bool queueHasHeadroom(const BackgroundGpuSignals& signals,
                        bool alreadyAllowed) const;

  BackgroundGpuAdmissionConfig config_;
  ForegroundPlaybackActivity previousForeground_ =
      ForegroundPlaybackActivity::Unavailable;
  Clock::time_point healthySince_ = Clock::time_point::min();
  Clock::time_point lastUpdate_ = Clock::time_point::min();
  bool allowed_ = false;
  bool lastForegroundStable_ = false;
  bool lastQueueHasHeadroom_ = false;
};

}  // namespace playback_session
