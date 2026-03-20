#pragma once

#include <cstdint>
#include <vector>

#include "radio.h"

struct PcmToIfPreviewModulator {
  float sampleRate = 0.0f;
  float audioBandwidthHz = 0.0f;
  float carrierHz = 0.0f;
  float carrierPhase = 0.0f;
  float fieldStrengthVoltsPerMeter = 0.0f;
  float antennaEffectiveHeightMeters = 0.0f;
  float modulationIndex = 0.0f;
  Biquad programHp;
  Biquad programLp1;
  Biquad programLp2;
  std::vector<float> monoScratch;

  void init(const Radio1938& radio, float newSampleRate, float bwHz);
  void reset();
  void processBlock(Radio1938& radio,
                    float* samples,
                    uint32_t frames,
                    uint32_t channels);
};
