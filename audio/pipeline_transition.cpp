#include "pipeline_transition.h"

#include <algorithm>
#include <cstddef>
#include <cmath>

uint32_t audioPipelineTransitionFrames(uint32_t sampleRate) {
  constexpr float kTransitionSeconds = 0.010f;
  const float rate = static_cast<float>(std::max(sampleRate, 1u));
  return std::max(1u, static_cast<uint32_t>(
                         std::lround(rate * kTransitionSeconds)));
}

void audioPipelineTransitionReset(AudioPipelineTransition& transition) {
  transition.phase.store(AudioPipelineTransitionPhase::Idle,
                         std::memory_order_relaxed);
  transition.requestSerial.store(0, std::memory_order_relaxed);
  transition.commitSerial.store(0, std::memory_order_relaxed);
  transition.inputFadeInRequestFrames.store(0, std::memory_order_relaxed);
  transition.outputFadeInRequestFrames.store(0, std::memory_order_relaxed);
  transition.inputFadeInFramesRemaining = 0;
  transition.inputFadeInFramesTotal = 0;
  transition.outputFadeInFramesRemaining = 0;
  transition.outputFadeInFramesTotal = 0;
}

static void requestFadeIn(std::atomic<uint32_t>& requestFrames,
                          uint32_t sampleRate) {
  const uint32_t requestedFrames = audioPipelineTransitionFrames(sampleRate);
  uint32_t current = requestFrames.load(std::memory_order_relaxed);
  while (current < requestedFrames &&
         !requestFrames.compare_exchange_weak(
             current, requestedFrames, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

void audioPipelineTransitionRequestOutputFadeIn(
    AudioPipelineTransition& transition,
    uint32_t sampleRate) {
  requestFadeIn(transition.outputFadeInRequestFrames, sampleRate);
}

void audioPipelineTransitionRequestSignalFadeIn(
    AudioPipelineTransition& transition,
    uint32_t sampleRate) {
  requestFadeIn(transition.inputFadeInRequestFrames, sampleRate);
  requestFadeIn(transition.outputFadeInRequestFrames, sampleRate);
}

void audioPipelineTransitionRequestDiscontinuity(
    AudioPipelineTransition& transition,
    uint32_t sampleRate) {
  transition.inputFadeInRequestFrames.store(0, std::memory_order_relaxed);
  transition.outputFadeInRequestFrames.store(0, std::memory_order_relaxed);
  transition.requestSerial.fetch_add(1, std::memory_order_relaxed);
  transition.phase.store(AudioPipelineTransitionPhase::FadeOutRequested,
                         std::memory_order_relaxed);
  (void)sampleRate;
}

bool audioPipelineTransitionBeginFadeOut(AudioPipelineTransition& transition) {
  AudioPipelineTransitionPhase expected =
      AudioPipelineTransitionPhase::FadeOutRequested;
  const bool started = transition.phase.compare_exchange_strong(
      expected, AudioPipelineTransitionPhase::CommitReady,
      std::memory_order_relaxed, std::memory_order_relaxed);
  if (started) {
    transition.commitSerial.store(
        transition.requestSerial.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
  }
  return started;
}

bool audioPipelineTransitionBeginCommit(AudioPipelineTransition& transition) {
  AudioPipelineTransitionPhase expected =
      AudioPipelineTransitionPhase::CommitReady;
  return transition.phase.compare_exchange_strong(
      expected, AudioPipelineTransitionPhase::CommitInProgress,
      std::memory_order_relaxed, std::memory_order_relaxed);
}

bool audioPipelineTransitionWaitingForCommit(
    const AudioPipelineTransition& transition) {
  const AudioPipelineTransitionPhase phase =
      transition.phase.load(std::memory_order_relaxed);
  return phase == AudioPipelineTransitionPhase::CommitReady ||
         phase == AudioPipelineTransitionPhase::CommitInProgress;
}

bool audioPipelineTransitionCommitInProgress(
    const AudioPipelineTransition& transition) {
  return transition.phase.load(std::memory_order_relaxed) ==
         AudioPipelineTransitionPhase::CommitInProgress;
}

bool audioPipelineTransitionActive(const AudioPipelineTransition& transition) {
  return transition.phase.load(std::memory_order_relaxed) !=
         AudioPipelineTransitionPhase::Idle;
}

bool audioPipelineTransitionFinishCommit(AudioPipelineTransition& transition) {
  const uint32_t committed =
      transition.commitSerial.load(std::memory_order_relaxed);
  const uint32_t requested =
      transition.requestSerial.load(std::memory_order_relaxed);
  if (committed != requested) {
    return false;
  }

  AudioPipelineTransitionPhase expected =
      AudioPipelineTransitionPhase::CommitInProgress;
  if (!transition.phase.compare_exchange_strong(
          expected, AudioPipelineTransitionPhase::Idle,
          std::memory_order_relaxed, std::memory_order_relaxed)) {
    expected = AudioPipelineTransitionPhase::CommitReady;
    if (!transition.phase.compare_exchange_strong(
            expected, AudioPipelineTransitionPhase::Idle,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
      return false;
    }
  }

  transition.commitSerial.store(0, std::memory_order_relaxed);
  return true;
}

static void beginFadeInIfRequested(std::atomic<uint32_t>& requestFrames,
                                   uint32_t& remainingFrames,
                                   uint32_t& totalFrames) {
  const uint32_t requestedFrames =
      requestFrames.exchange(0, std::memory_order_relaxed);
  if (requestedFrames == 0) return;
  remainingFrames = requestedFrames;
  totalFrames = requestedFrames;
}

static float nextFadeInGain(uint32_t& remainingFrames,
                            uint32_t& totalFrames) {
  if (remainingFrames == 0 || totalFrames == 0) {
    remainingFrames = 0;
    totalFrames = 0;
    return 1.0f;
  }

  const uint32_t total = std::max(totalFrames, 1u);
  const uint32_t remaining = std::min(remainingFrames, total);
  const uint32_t frameIndex = total - remaining;
  const float gain =
      static_cast<float>(frameIndex + 1) / static_cast<float>(total);
  remainingFrames = remaining - 1;
  if (remainingFrames == 0) {
    totalFrames = 0;
  }
  return gain;
}

static void applyFadeIn(std::atomic<uint32_t>& requestFrames,
                        uint32_t& remainingFrames,
                        uint32_t& totalFrames,
                        float* samples,
                        uint32_t frames,
                        uint32_t channels) {
  if (!samples || frames == 0 || channels == 0) return;
  beginFadeInIfRequested(requestFrames, remainingFrames, totalFrames);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    const float gain = nextFadeInGain(remainingFrames, totalFrames);
    if (gain >= 1.0f) continue;
    const size_t frameOffset = static_cast<size_t>(frame) * channels;
    for (uint32_t ch = 0; ch < channels; ++ch) {
      samples[frameOffset + ch] *= gain;
    }
  }
}

void audioPipelineTransitionApplyInputFadeIn(
    AudioPipelineTransition& transition,
    float* samples,
    uint32_t frames,
    uint32_t channels) {
  applyFadeIn(transition.inputFadeInRequestFrames,
              transition.inputFadeInFramesRemaining,
              transition.inputFadeInFramesTotal, samples, frames, channels);
}

void audioPipelineTransitionClearInputFadeIn(
    AudioPipelineTransition& transition) {
  transition.inputFadeInRequestFrames.store(0, std::memory_order_relaxed);
  transition.inputFadeInFramesRemaining = 0;
  transition.inputFadeInFramesTotal = 0;
}

void audioPipelineTransitionApplyFadeIn(AudioPipelineTransition& transition,
                                        float* samples,
                                        uint32_t frames,
                                        uint32_t channels) {
  applyFadeIn(transition.outputFadeInRequestFrames,
              transition.outputFadeInFramesRemaining,
              transition.outputFadeInFramesTotal, samples, frames, channels);
}

void audioPipelineTransitionFadeOutToSilence(float* samples,
                                             uint32_t frames,
                                             uint32_t channels,
                                             uint32_t sampleRate) {
  if (!samples || frames == 0 || channels == 0) return;
  const uint32_t fadeFrames =
      std::min(frames, audioPipelineTransitionFrames(sampleRate));
  for (uint32_t frame = 0; frame < frames; ++frame) {
    float gain = 0.0f;
    if (frame < fadeFrames) {
      gain = static_cast<float>(fadeFrames - frame - 1) /
             static_cast<float>(fadeFrames);
    }
    const size_t frameOffset = static_cast<size_t>(frame) * channels;
    for (uint32_t ch = 0; ch < channels; ++ch) {
      samples[frameOffset + ch] *= gain;
    }
  }
}

void audioPipelineTransitionFadeTailToSilence(float* samples,
                                              uint32_t audioFrames,
                                              uint32_t channels,
                                              uint32_t sampleRate) {
  if (!samples || audioFrames == 0 || channels == 0) return;
  const uint32_t fadeFrames =
      std::min(audioFrames, audioPipelineTransitionFrames(sampleRate));
  const uint32_t firstFadeFrame = audioFrames - fadeFrames;
  for (uint32_t frame = 0; frame < fadeFrames; ++frame) {
    const float gain = static_cast<float>(fadeFrames - frame - 1) /
                       static_cast<float>(fadeFrames);
    const size_t frameOffset =
        static_cast<size_t>(firstFadeFrame + frame) * channels;
    for (uint32_t ch = 0; ch < channels; ++ch) {
      samples[frameOffset + ch] *= gain;
    }
  }
}
