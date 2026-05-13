#include "output_transition.h"

#include <algorithm>
#include <cstddef>
#include <cmath>

uint32_t audioOutputTransitionFrames(uint32_t sampleRate) {
  constexpr float kTransitionSeconds = 0.010f;
  const float rate = static_cast<float>(std::max(sampleRate, 1u));
  return std::max(1u, static_cast<uint32_t>(
                         std::lround(rate * kTransitionSeconds)));
}

void audioOutputTransitionReset(AudioOutputTransition& transition) {
  transition.phase.store(AudioOutputTransitionPhase::Idle,
                         std::memory_order_relaxed);
  transition.requestSerial.store(0, std::memory_order_relaxed);
  transition.commitSerial.store(0, std::memory_order_relaxed);
  transition.fadeInRequestFrames.store(0, std::memory_order_relaxed);
  transition.fadeInFramesRemaining = 0;
  transition.fadeInFramesTotal = 0;
}

void audioOutputTransitionRequestFadeIn(AudioOutputTransition& transition,
                                        uint32_t sampleRate) {
  const uint32_t requestedFrames = audioOutputTransitionFrames(sampleRate);
  uint32_t current =
      transition.fadeInRequestFrames.load(std::memory_order_relaxed);
  while (current < requestedFrames &&
         !transition.fadeInRequestFrames.compare_exchange_weak(
             current, requestedFrames, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

void audioOutputTransitionRequestDiscontinuity(AudioOutputTransition& transition,
                                               uint32_t sampleRate) {
  transition.fadeInRequestFrames.store(0, std::memory_order_relaxed);
  transition.requestSerial.fetch_add(1, std::memory_order_relaxed);
  transition.phase.store(AudioOutputTransitionPhase::FadeOutRequested,
                         std::memory_order_relaxed);
  audioOutputTransitionRequestFadeIn(transition, sampleRate);
}

bool audioOutputTransitionBeginFadeOut(AudioOutputTransition& transition) {
  AudioOutputTransitionPhase expected =
      AudioOutputTransitionPhase::FadeOutRequested;
  const bool started = transition.phase.compare_exchange_strong(
      expected, AudioOutputTransitionPhase::CommitReady,
      std::memory_order_relaxed, std::memory_order_relaxed);
  if (started) {
    transition.commitSerial.store(
        transition.requestSerial.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
  }
  return started;
}

bool audioOutputTransitionBeginCommit(AudioOutputTransition& transition) {
  AudioOutputTransitionPhase expected = AudioOutputTransitionPhase::CommitReady;
  return transition.phase.compare_exchange_strong(
      expected, AudioOutputTransitionPhase::CommitInProgress,
      std::memory_order_relaxed, std::memory_order_relaxed);
}

bool audioOutputTransitionWaitingForCommit(
    const AudioOutputTransition& transition) {
  const AudioOutputTransitionPhase phase =
      transition.phase.load(std::memory_order_relaxed);
  return phase == AudioOutputTransitionPhase::CommitReady ||
         phase == AudioOutputTransitionPhase::CommitInProgress;
}

bool audioOutputTransitionActive(const AudioOutputTransition& transition) {
  return transition.phase.load(std::memory_order_relaxed) !=
         AudioOutputTransitionPhase::Idle;
}

bool audioOutputTransitionFinishCommit(AudioOutputTransition& transition,
                                       uint32_t sampleRate) {
  const uint32_t committed =
      transition.commitSerial.load(std::memory_order_relaxed);
  const uint32_t requested =
      transition.requestSerial.load(std::memory_order_relaxed);
  if (committed != requested) {
    return false;
  }

  AudioOutputTransitionPhase expected =
      AudioOutputTransitionPhase::CommitInProgress;
  if (!transition.phase.compare_exchange_strong(
          expected, AudioOutputTransitionPhase::Idle, std::memory_order_relaxed,
          std::memory_order_relaxed)) {
    expected = AudioOutputTransitionPhase::CommitReady;
    if (!transition.phase.compare_exchange_strong(
            expected, AudioOutputTransitionPhase::Idle,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
      return false;
    }
  }

  transition.commitSerial.store(0, std::memory_order_relaxed);
  audioOutputTransitionRequestFadeIn(transition, sampleRate);
  return true;
}

static void beginFadeInIfRequested(AudioOutputTransition& transition) {
  const uint32_t requestedFrames =
      transition.fadeInRequestFrames.exchange(0, std::memory_order_relaxed);
  if (requestedFrames == 0) return;
  transition.fadeInFramesRemaining = requestedFrames;
  transition.fadeInFramesTotal = requestedFrames;
}

static float nextFadeInGain(AudioOutputTransition& transition) {
  if (transition.fadeInFramesRemaining == 0 ||
      transition.fadeInFramesTotal == 0) {
    transition.fadeInFramesRemaining = 0;
    transition.fadeInFramesTotal = 0;
    return 1.0f;
  }

  const uint32_t total = std::max(transition.fadeInFramesTotal, 1u);
  const uint32_t remaining =
      std::min(transition.fadeInFramesRemaining, total);
  const uint32_t frameIndex = total - remaining;
  const float gain =
      static_cast<float>(frameIndex + 1) / static_cast<float>(total);
  transition.fadeInFramesRemaining = remaining - 1;
  if (transition.fadeInFramesRemaining == 0) {
    transition.fadeInFramesTotal = 0;
  }
  return std::clamp(gain, 0.0f, 1.0f);
}

void audioOutputTransitionApplyFadeIn(AudioOutputTransition& transition,
                                      float* samples,
                                      uint32_t frames,
                                      uint32_t channels) {
  if (!samples || frames == 0 || channels == 0) return;
  beginFadeInIfRequested(transition);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    const float gain = nextFadeInGain(transition);
    if (gain >= 1.0f) continue;
    const size_t frameOffset = static_cast<size_t>(frame) * channels;
    for (uint32_t ch = 0; ch < channels; ++ch) {
      samples[frameOffset + ch] *= gain;
    }
  }
}

void audioOutputTransitionFadeOutToSilence(float* samples,
                                           uint32_t frames,
                                           uint32_t channels,
                                           uint32_t sampleRate) {
  if (!samples || frames == 0 || channels == 0) return;
  const uint32_t fadeFrames =
      std::min(frames, audioOutputTransitionFrames(sampleRate));
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

void audioOutputTransitionFadeTailToSilence(float* samples,
                                            uint32_t audioFrames,
                                            uint32_t channels,
                                            uint32_t sampleRate) {
  if (!samples || audioFrames == 0 || channels == 0) return;
  const uint32_t fadeFrames =
      std::min(audioFrames, audioOutputTransitionFrames(sampleRate));
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
