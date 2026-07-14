#pragma once

#include <cstdint>

class RadioBypassTransition {
 public:
  void reset(bool radioEnabled, uint32_t sampleRate);

  bool needsWetSignal(bool radioEnabled) const;
  uint32_t blendFramesRemaining(bool radioEnabled,
                                uint32_t sampleRate) const;
  bool activateWetSignal(bool radioEnabled);
  bool requestWetReplacement();
  void cancelWetReplacement();
  bool wetReplacementActive() const;
  bool wetReplacementReadyToCommit() const;
  bool commitWetReplacement();

  void blend(float* wetSamples,
             const float* drySamples,
             uint32_t frames,
             uint32_t channels,
             bool radioEnabled,
             uint32_t sampleRate);

 private:
  enum class WetReplacementPhase : uint8_t {
    None,
    FadeOut,
    FadeIn,
  };

  uint32_t wetFrames_ = 0;
  bool wetSignalActive_ = false;
  WetReplacementPhase wetReplacementPhase_ = WetReplacementPhase::None;
};
