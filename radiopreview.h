#pragma once

#include <cstdint>
#include <vector>

#include "radio.h"

struct PcmToIfPreviewModulator {
  float sampleRate = 0.0f;
  float ifLowHz = 0.0f;
  float ifHighHz = 0.0f;
  float audioBandwidthHz = 0.0f;
  float carrierHz = 0.0f;
  float phase = 0.0f;
  float carrierAmplitude = 0.0f;
  float modulationIndex = 0.0f;
  float modulationLimit = 0.0f;
  float programLevelEnv = 0.0f;
  float programLevelAtk = 0.0f;
  float programLevelRel = 0.0f;
  float modulationRef = 0.0f;
  Biquad programHp;
  Biquad programLp1;
  Biquad programLp2;
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
