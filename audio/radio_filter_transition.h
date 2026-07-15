#pragma once

#include <cstdint>

#include "radio_filter_mode.h"

class RadioFilterTransition {
 public:
  void reset(RadioFilterMode mode, uint32_t sampleRate);

  bool retarget(RadioFilterMode mode);
  bool commitAtDryBoundary();

  RadioFilterMode activeMode() const { return activeMode_; }
  bool needsWetSignal() const;
  bool blending() const;
  uint32_t framesUntilBoundary(uint32_t sampleRate) const;

  void blend(float* wetSamples,
             const float* drySamples,
             uint32_t frames,
             uint32_t channels,
             uint32_t sampleRate);

 private:
  enum class Phase : uint8_t {
    Stable,
    FadeOut,
    FadeIn,
  };

  RadioFilterMode activeMode_ = RadioFilterMode::Off;
  RadioFilterMode targetMode_ = RadioFilterMode::Off;
  uint32_t wetFrames_ = 0;
  Phase phase_ = Phase::Stable;
};
