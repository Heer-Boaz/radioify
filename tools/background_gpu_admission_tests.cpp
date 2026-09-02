#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>

#include "playback/session/background_gpu_admission.h"

namespace {

bool expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

playback_session::BackgroundGpuSignals activeSignals(
    std::size_t queueDepth, std::int64_t frameDurationUs = 33'333) {
  playback_session::BackgroundGpuSignals signals;
  signals.foreground = playback_session::ForegroundPlaybackActivity::Active;
  signals.hasVideoFrame = true;
  signals.videoQueueDepth = queueDepth;
  signals.lastPresentedDurationUs = frameDurationUs;
  return signals;
}

}  // namespace

int main() {
  using namespace std::chrono_literals;
  using playback_session::BackgroundGpuAdmissionPolicy;
  using playback_session::BackgroundGpuSignals;
  using playback_session::ForegroundPlaybackActivity;

  bool ok = true;
  const auto start = BackgroundGpuAdmissionPolicy::Clock::time_point{};

  BackgroundGpuAdmissionPolicy policy;
  auto decision = policy.update(activeSignals(3), start);
  ok &= expect(!decision.allowed && !decision.changed &&
                   decision.foregroundStable && decision.queueHasHeadroom &&
                   decision.healthyFor == 0ms,
               "active playback must establish a stable admission window");
  ok &= expect(policy.nextEvaluationDeadline() == start + 1500ms,
               "a healthy queue must schedule the end of its admission delay");

  decision = policy.update(activeSignals(3), start + 1499ms);
  ok &= expect(!decision.allowed && !decision.changed,
               "background GPU work must not start before the full delay");
  decision = policy.update(activeSignals(3), start + 1500ms);
  ok &= expect(decision.allowed && decision.changed,
               "stable buffered playback must admit background GPU work");
  ok &= expect(!policy.nextEvaluationDeadline(),
               "an admitted worker must not leave a stale wake deadline");

  decision = policy.update(activeSignals(2), start + 1600ms);
  ok &= expect(decision.allowed && !decision.changed,
               "the lower retain watermark must absorb normal queue jitter");
  decision = policy.update(activeSignals(1), start + 1700ms);
  ok &=
      expect(!decision.allowed && decision.changed,
             "foreground pressure must revoke an active GPU lease immediately");

  decision = policy.update(activeSignals(3), start + 2000ms);
  ok &= expect(!decision.allowed && !decision.changed &&
                   policy.nextEvaluationDeadline() == start + 3200ms,
               "a revoked lease must observe one cooldown before re-admission");
  decision = policy.update(activeSignals(2), start + 3300ms);
  ok &= expect(!decision.allowed && !policy.nextEvaluationDeadline(),
               "an expired cooldown without the high watermark must wait for "
               "new playback evidence instead of busy-waking");
  decision = policy.update(activeSignals(3), start + 3400ms);
  ok &= expect(decision.allowed && decision.changed,
               "a recovered high watermark must re-admit after the cooldown");

  BackgroundGpuSignals starved = activeSignals(16);
  starved.audioStarved = true;
  decision = policy.update(starved, start + 3500ms);
  ok &=
      expect(!decision.allowed && decision.changed &&
                 !decision.foregroundStable && !policy.nextEvaluationDeadline(),
             "audio starvation must preempt background GPU work");
  decision = policy.update(activeSignals(16), start + 3600ms);
  ok &= expect(
      !decision.allowed && policy.nextEvaluationDeadline() == start + 5100ms,
      "recovery from foreground instability must start a new delay");

  BackgroundGpuAdmissionPolicy pausedPolicy;
  BackgroundGpuSignals inactive;
  inactive.foreground = ForegroundPlaybackActivity::Inactive;
  decision = pausedPolicy.update(inactive, start);
  ok &= expect(decision.allowed && decision.changed,
               "paused or ended playback must release the GPU immediately");
  decision = pausedPolicy.update(activeSignals(16), start + 1ms);
  ok &= expect(!decision.allowed && decision.changed,
               "resuming playback must reclaim the GPU for a fresh stable "
               "admission window");

  BackgroundGpuAdmissionPolicy fallbackPolicy;
  decision = fallbackPolicy.update(activeSignals(3, 0), start);
  ok &= expect(!decision.queueHasHeadroom,
               "unknown frame duration must use the conservative frame "
               "watermark");
  decision = fallbackPolicy.update(activeSignals(4, 0), start + 1ms);
  ok &= expect(decision.queueHasHeadroom,
               "the fallback high watermark must admit a sufficiently deep "
               "queue");

  BackgroundGpuSignals unavailable;
  unavailable.foreground = ForegroundPlaybackActivity::Unavailable;
  decision = pausedPolicy.update(unavailable, start + 2ms);
  ok &= expect(!decision.allowed,
               "an unavailable foreground state must keep background GPU work "
               "out");

  return ok ? 0 : 1;
}
