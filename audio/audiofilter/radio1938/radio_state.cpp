#include "../../radio.h"
#include "../math/signal_math.h"
#include "radio_buffer_io.h"

#include <algorithm>
#include <cmath>

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

  const uint32_t batchFrames = std::min<uint32_t>(warmupFrames, 4096u);
  auto& warmup = iqInput.rfScratch;
  const size_t batchSamples = static_cast<size_t>(batchFrames) * channels;
  if (warmup.size() < batchSamples) warmup.resize(batchSamples);
  const float carrierPeak =
      std::sqrt(2.0f) * std::max(receivedCarrierRmsVolts, 0.0f);
  const float carrierHz = resolvedInputCarrierHz();
  const float safeSampleRate = std::max(sampleRate, 1.0f);
  const float carrierStep = kRadioTwoPi * (carrierHz / safeSampleRate);
  const float carrierStepCos = std::cos(carrierStep);
  const float carrierStepSin = std::sin(carrierStep);
  auto& carrierCos = iqInput.carrierOscCos;
  auto& carrierSin = iqInput.carrierOscSin;
  uint32_t remaining = warmupFrames;

  while (remaining > 0u) {
    const uint32_t batch =
        std::min<uint32_t>(remaining, batchFrames);
    for (uint32_t frame = 0; frame < batch; ++frame) {
      writeMonoToInterleaved(warmup.data(), frame, channels,
                             carrierPeak * carrierCos);
      advanceNormalizedOscillator(carrierStepCos, carrierStepSin, carrierCos,
                                  carrierSin);
    }
    processIfReal(warmup.data(), batch);
    remaining -= batch;
  }

  resetAfterCarrierWarmup();
}

void Radio1938::resetAfterCarrierWarmup() {
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
