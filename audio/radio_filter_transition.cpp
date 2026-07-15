#include "radio_filter_transition.h"

#include <cassert>
#include <cstddef>
#include <cstdlib>

#include "pipeline_transition.h"

void RadioFilterTransition::reset(RadioFilterMode mode, uint32_t sampleRate) {
  activeMode_ = mode;
  targetMode_ = mode;
  wetFrames_ = radioFilterModeEnabled(mode)
                   ? audioPipelineTransitionFrames(sampleRate)
                   : 0;
  phase_ = Phase::Stable;
}

bool RadioFilterTransition::retarget(RadioFilterMode mode) {
  if (mode == targetMode_) return false;
  targetMode_ = mode;

  switch (phase_) {
    case Phase::Stable:
      if (!radioFilterModeEnabled(activeMode_)) {
        if (!radioFilterModeEnabled(targetMode_)) return false;
        activeMode_ = targetMode_;
        phase_ = Phase::FadeIn;
        return true;
      }
      if (targetMode_ != activeMode_) {
        phase_ = Phase::FadeOut;
      }
      return false;

    case Phase::FadeOut:
      if (targetMode_ == activeMode_) {
        phase_ = Phase::FadeIn;
      }
      return false;

    case Phase::FadeIn:
      if (targetMode_ != activeMode_) {
        phase_ = Phase::FadeOut;
      }
      return false;
  }

  std::abort();
}

bool RadioFilterTransition::commitAtDryBoundary() {
  if (phase_ != Phase::FadeOut || wetFrames_ != 0) return false;
  if (!radioFilterModeEnabled(targetMode_)) {
    activeMode_ = RadioFilterMode::Off;
    phase_ = Phase::Stable;
    return false;
  }
  if (activeMode_ == targetMode_) {
    phase_ = Phase::FadeIn;
    return false;
  }
  activeMode_ = targetMode_;
  phase_ = Phase::FadeIn;
  return true;
}

bool RadioFilterTransition::needsWetSignal() const {
  return radioFilterModeEnabled(activeMode_);
}

bool RadioFilterTransition::blending() const {
  return phase_ != Phase::Stable;
}

uint32_t RadioFilterTransition::framesUntilBoundary(
    uint32_t sampleRate) const {
  const uint32_t transitionFrames =
      audioPipelineTransitionFrames(sampleRate);
  switch (phase_) {
    case Phase::Stable:
      return 0;
    case Phase::FadeOut:
      return wetFrames_;
    case Phase::FadeIn:
      return transitionFrames - wetFrames_;
  }
  std::abort();
}

void RadioFilterTransition::blend(float* wetSamples,
                                  const float* drySamples,
                                  uint32_t frames,
                                  uint32_t channels,
                                  uint32_t sampleRate) {
  assert(wetSamples && drySamples && frames > 0 && channels > 0);
  assert(blending());
  assert(frames <= framesUntilBoundary(sampleRate));
  const uint32_t transitionFrames =
      audioPipelineTransitionFrames(sampleRate);
  const bool fadingIn = phase_ == Phase::FadeIn;

  for (uint32_t frame = 0; frame < frames; ++frame) {
    const float wetMix = static_cast<float>(wetFrames_) /
                         static_cast<float>(transitionFrames);
    const size_t frameOffset = static_cast<size_t>(frame) * channels;
    for (uint32_t channel = 0; channel < channels; ++channel) {
      const size_t sample = frameOffset + channel;
      wetSamples[sample] = drySamples[sample] * (1.0f - wetMix) +
                           wetSamples[sample] * wetMix;
    }

    if (fadingIn) {
      if (wetFrames_ < transitionFrames) ++wetFrames_;
    } else if (wetFrames_ > 0) {
      --wetFrames_;
    }
  }

  if (phase_ == Phase::FadeIn && wetFrames_ == transitionFrames) {
    phase_ = Phase::Stable;
  }
}
