#include "radio_bypass_transition.h"

#include <cstddef>

#include "pipeline_transition.h"

void RadioBypassTransition::reset(bool radioEnabled, uint32_t sampleRate) {
  wetFrames_ = radioEnabled ? audioPipelineTransitionFrames(sampleRate) : 0;
  wetSignalActive_ = radioEnabled;
}

bool RadioBypassTransition::needsWetSignal(bool radioEnabled) const {
  return radioEnabled || wetFrames_ > 0;
}

uint32_t RadioBypassTransition::blendFramesRemaining(
    bool radioEnabled,
    uint32_t sampleRate) const {
  const uint32_t transitionFrames =
      audioPipelineTransitionFrames(sampleRate);
  return radioEnabled ? transitionFrames - wetFrames_ : wetFrames_;
}

bool RadioBypassTransition::activateWetSignal(bool radioEnabled) {
  if (!needsWetSignal(radioEnabled) || wetSignalActive_) return false;
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
  for (uint32_t frame = 0; frame < frames; ++frame) {
    const float wetMix = static_cast<float>(wetFrames_) /
                         static_cast<float>(transitionFrames);
    const size_t frameOffset = static_cast<size_t>(frame) * channels;
    for (uint32_t channel = 0; channel < channels; ++channel) {
      const size_t sample = frameOffset + channel;
      wetSamples[sample] = drySamples[sample] * (1.0f - wetMix) +
                           wetSamples[sample] * wetMix;
    }

    if (radioEnabled) {
      if (wetFrames_ < transitionFrames) ++wetFrames_;
    } else if (wetFrames_ > 0) {
      --wetFrames_;
    }
  }

  if (!radioEnabled && wetFrames_ == 0) {
    wetSignalActive_ = false;
  }
}
