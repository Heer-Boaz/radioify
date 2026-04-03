#include "../../radio.h"

#include <algorithm>
#include <cmath>
#include <vector>

float Radio1938::resolvedInputCarrierHz() const {
  const float safeSampleRate = std::max(sampleRate, 1.0f);
  return std::clamp(tuning.sourceCarrierHz, 1000.0f, safeSampleRate * 0.45f);
}

void Radio1938::warmInputCarrier(float receivedCarrierRmsVolts,
                                 uint32_t warmupFrames) {
  if (warmupFrames == 0u) {
    resetAfterCarrierWarmup();
    return;
  }

  std::vector<float> warmup(std::min<uint32_t>(warmupFrames, 4096u), 0.0f);
  const float carrierPeak =
      std::sqrt(2.0f) * std::max(receivedCarrierRmsVolts, 0.0f);
  const float carrierHz = resolvedInputCarrierHz();
  const float safeSampleRate = std::max(sampleRate, 1.0f);
  const float carrierStep = kRadioTwoPi * (carrierHz / safeSampleRate);
  float phase = iqInput.iqPhase;
  uint32_t remaining = warmupFrames;

  while (remaining > 0u) {
    const uint32_t batch =
        std::min<uint32_t>(remaining, static_cast<uint32_t>(warmup.size()));
    for (uint32_t frame = 0; frame < batch; ++frame) {
      warmup[frame] = carrierPeak * std::cos(phase);
      phase += carrierStep;
      if (phase >= kRadioTwoPi) phase -= kRadioTwoPi;
    }
    processIfReal(warmup.data(), batch);
    remaining -= batch;
  }

  iqInput.iqPhase = phase;
  resetAfterCarrierWarmup();
}

void Radio1938::resetAfterCarrierWarmup() {
  demod.am.avcRect = 0.0f;
  demod.am.avcEnv = 0.0f;
  demod.am.afcError = 0.0f;
  controlSense.reset();
  controlBus.reset();
  tuning.afcCorrectionHz = 0.0f;
  diagnostics.reset();
  if (calibration.enabled) {
    resetCalibration();
  }
}
