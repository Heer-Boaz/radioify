#pragma once

#include <cstdint>
#include <vector>

#include "radio.h"

struct PcmToIfPreviewModulator {
  float sampleRate = 0.0f;
  float carrierHz = 0.0f;
  float phase = 0.0f;
  float carrierLevel = 0.82f;
  float modulationDepth = 0.55f;
  float minEnvelope = 0.08f;
  Biquad programHp;
  Biquad programLp;
  std::vector<float> monoScratch;

  void init(const Radio1938& radio, float newSampleRate, float bwHz);
  void reset();
  void processBlock(Radio1938& radio,
                    float* samples,
                    uint32_t frames,
                    uint32_t channels,
                    float makeupGain);
};

bool radioPreviewBlockOverloaded(const Radio1938& radio, uint32_t frames);
