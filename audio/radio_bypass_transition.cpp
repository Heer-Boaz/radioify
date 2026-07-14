#include "radio_bypass_transition.h"

#include <cstddef>

#include "pipeline_transition.h"

void RadioBypassTransition::reset(bool radioEnabled, uint32_t sampleRate) {
  wetFrames_ = radioEnabled ? audioPipelineTransitionFrames(sampleRate) : 0;
  wetSignalActive_ = radioEnabled;
  wetReplacementPhase_ = WetReplacementPhase::None;
}

bool RadioBypassTransition::needsWetSignal(bool radioEnabled) const {
  return radioEnabled || wetFrames_ > 0 || wetReplacementActive();
}

uint32_t RadioBypassTransition::blendFramesRemaining(
    bool radioEnabled,
    uint32_t sampleRate) const {
  const uint32_t transitionFrames =
      audioPipelineTransitionFrames(sampleRate);
  if (wetReplacementPhase_ == WetReplacementPhase::FadeOut) {
    return wetFrames_;
  }
  if (wetReplacementPhase_ == WetReplacementPhase::FadeIn) {
    return transitionFrames - wetFrames_;
  }
  return radioEnabled ? transitionFrames - wetFrames_ : wetFrames_;
}

bool RadioBypassTransition::activateWetSignal(bool radioEnabled) {
  if (!needsWetSignal(radioEnabled) || wetSignalActive_) return false;
  wetSignalActive_ = true;
  return true;
}

bool RadioBypassTransition::requestWetReplacement() {
  if (!wetSignalActive_ || wetReplacementActive()) return false;
  wetReplacementPhase_ = WetReplacementPhase::FadeOut;
  return true;
}

void RadioBypassTransition::cancelWetReplacement() {
  wetReplacementPhase_ = WetReplacementPhase::None;
}

bool RadioBypassTransition::wetReplacementActive() const {
  return wetReplacementPhase_ != WetReplacementPhase::None;
}

bool RadioBypassTransition::wetReplacementReadyToCommit() const {
  return wetReplacementPhase_ == WetReplacementPhase::FadeOut &&
         wetFrames_ == 0;
}

bool RadioBypassTransition::commitWetReplacement() {
  if (!wetReplacementReadyToCommit()) return false;
  wetReplacementPhase_ = WetReplacementPhase::FadeIn;
  wetSignalActive_ = true;
  return true;
}

void RadioBypassTransition::blend(float* wetSamples,
                                  const float* drySamples,
                                  uint32_t frames,
                                  uint32_t channels,
                                  bool radioEnabled,
                                  uint32_t sampleRate) {
  const uint32_t transitionFrames =
      audioPipelineTransitionFrames(sampleRate);
  const bool targetWet =
      wetReplacementPhase_ == WetReplacementPhase::FadeOut ? false
                                                            : radioEnabled;
  for (uint32_t frame = 0; frame < frames; ++frame) {
    const float wetMix = static_cast<float>(wetFrames_) /
                         static_cast<float>(transitionFrames);
    const size_t frameOffset = static_cast<size_t>(frame) * channels;
    for (uint32_t channel = 0; channel < channels; ++channel) {
      const size_t sample = frameOffset + channel;
      wetSamples[sample] = drySamples[sample] * (1.0f - wetMix) +
                           wetSamples[sample] * wetMix;
    }

    if (targetWet) {
      if (wetFrames_ < transitionFrames) ++wetFrames_;
    } else if (wetFrames_ > 0) {
      --wetFrames_;
    }
  }

  if (wetReplacementPhase_ == WetReplacementPhase::FadeIn &&
      wetFrames_ == transitionFrames) {
    wetReplacementPhase_ = WetReplacementPhase::None;
  }
  if (!radioEnabled && !wetReplacementActive() && wetFrames_ == 0) {
    wetSignalActive_ = false;
  }
}
