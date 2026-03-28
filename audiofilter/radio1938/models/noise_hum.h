#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_NOISE_HUM_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_NOISE_HUM_H

#include "../../math/biquad.h"

#include <random>

struct NoiseInput {
  float programSample = 0.0f;
  float noiseAmp = 0.0f;
  float crackleAmp = 0.0f;
  float crackleRate = 0.0f;
  float humAmp = 0.0f;
  bool humToneEnabled = true;
};

struct NoiseHum {
  std::mt19937 rng{0x1938u};
  std::uniform_real_distribution<float> dist{-1.0f, 1.0f};
  std::uniform_real_distribution<float> dist01{0.0f, 1.0f};
  Biquad hp;
  Biquad lp;
  Biquad crackleHp;
  Biquad crackleLp;
  float fs = 0.0f;
  float noiseHpHz = 0.0f;
  float noiseLpHz = 0.0f;
  float humHz = 0.0f;
  float humPhase = 0.0f;
  float scEnv = 0.0f;
  float scAtk = 0.0f;
  float scRel = 0.0f;
  float crackleEnv = 0.0f;
  float crackleDecay = 0.0f;
  float pinkFast = 0.0f;
  float pinkSlow = 0.0f;
  float brown = 0.0f;
  float hissDrift = 0.0f;
  float hissDriftSlow = 0.0f;
  float filterQ = 0.0f;
  float scAttackMs = 0.0f;
  float scReleaseMs = 0.0f;
  float crackleDecayMs = 0.0f;
  float sidechainMaskRef = 0.0f;
  float hissMaskDepth = 0.0f;
  float burstMaskDepth = 0.0f;
  float pinkFastPole = 0.0f;
  float pinkSlowPole = 0.0f;
  float brownStep = 0.0f;
  float hissDriftPole = 0.0f;
  float hissDriftNoise = 0.0f;
  float hissDriftSlowPole = 0.0f;
  float hissDriftSlowNoise = 0.0f;
  float whiteMix = 0.0f;
  float pinkFastMix = 0.0f;
  float pinkDifferenceMix = 0.0f;
  float pinkFastSubtract = 0.0f;
  float brownMix = 0.0f;
  float hissBase = 0.0f;
  float hissDriftDepth = 0.0f;
  float hissDriftSlowMix = 0.0f;
  float humSecondHarmonicMix = 0.0f;
};

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_NOISE_HUM_H
