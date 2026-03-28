#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cmath>

namespace {

float clampPublishedBandwidth(const Radio1938& radio, float bwHz) {
  const auto& tuning = radio.tuning;
  float safeBwMinHz = std::max(tuning.safeBwMinHz, 1.0f);
  float safeBwMaxHz = std::max(safeBwMinHz, tuning.safeBwMaxHz);
  return std::clamp(bwHz, safeBwMinHz, safeBwMaxHz);
}

float selectSourceCarrierHz(float outputFs,
                            float internalFs,
                            float ifCenterHz,
                            float bwHz) {
  float maxCarrierByIf = 0.48f * internalFs - ifCenterHz - 8000.0f;
  float audioSidebandHz = 0.48f * std::max(bwHz, 1.0f);
  float maxCarrierByOutput =
      0.5f * std::max(outputFs, 1.0f) - audioSidebandHz - 1600.0f;
  float maxCarrier = maxCarrierByOutput;
  if (ifCenterHz > 0.0f) {
    maxCarrier = std::min(maxCarrierByIf, maxCarrierByOutput);
  }
  if (maxCarrier <= 6000.0f) {
    return std::clamp(0.25f * std::max(outputFs, 1.0f), 3000.0f,
                      std::max(3000.0f, maxCarrierByOutput));
  }
  return std::clamp(0.62f * maxCarrier, 6000.0f, maxCarrier);
}

float publishTuningConfig(Radio1938& radio, float tuneHz, float bwHz) {
  auto& tuning = radio.tuning;
  float safeAudioBw = clampPublishedBandwidth(radio, bwHz);
  float physicalChannelBw = 2.0f * safeAudioBw;
  float sourceCarrierHz = selectSourceCarrierHz(
      radio.sampleRate, radio.sampleRate, 0.0f, physicalChannelBw);
  bool configChanged =
      tuning.configRevision == 0 ||
      std::fabs(tuneHz - tuning.tuneAppliedHz) > tuning.updateEps ||
      std::fabs(safeAudioBw - tuning.bwAppliedHz) > tuning.updateEps ||
      std::fabs(sourceCarrierHz - tuning.sourceCarrierHz) > 1e-3f;
  tuning.tunedBw = safeAudioBw;
  tuning.tuneAppliedHz = tuneHz;
  tuning.bwAppliedHz = safeAudioBw;
  tuning.sourceCarrierHz = sourceCarrierHz;
  if (configChanged) {
    ++tuning.configRevision;
  }
  return safeAudioBw;
}

}  // namespace

void RadioTuningNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& tuning = radio.tuning;
  tuning.afcCorrectionHz = 0.0f;
  tuning.configRevision = 0;
  tuning.tuneSmoothedHz = tuning.tuneOffsetHz;
  tuning.bwSmoothedHz = clampPublishedBandwidth(radio, radio.bwHz);
  initCtx.tunedBw =
      publishTuningConfig(radio, tuning.tuneSmoothedHz, tuning.bwSmoothedHz);
}

void RadioTuningNode::reset(Radio1938& radio) {
  auto& tuning = radio.tuning;
  tuning.afcCorrectionHz = 0.0f;
  tuning.tuneSmoothedHz = tuning.tuneAppliedHz;
  tuning.bwSmoothedHz = tuning.bwAppliedHz;
}

void RadioTuningNode::prepare(Radio1938& radio,
                              RadioBlockControl& block,
                              uint32_t frames) {
  auto& tuning = radio.tuning;
  float rate = std::max(1.0f, radio.sampleRate);
  float smoothTau = std::max(tuning.smoothTau, 1e-4f);
  float tick =
      1.0f - std::exp(-static_cast<float>(frames) / (rate * smoothTau));
  float effectiveTuneHz = tuning.tuneOffsetHz;
  if (tuning.magneticTuningEnabled) {
    effectiveTuneHz += tuning.afcCorrectionHz;
  }
  float targetBwHz = clampPublishedBandwidth(radio, radio.bwHz);
  tuning.tuneSmoothedHz += tick * (effectiveTuneHz - tuning.tuneSmoothedHz);
  tuning.bwSmoothedHz += tick * (targetBwHz - tuning.bwSmoothedHz);

  float safeBw = clampPublishedBandwidth(radio, tuning.bwSmoothedHz);
  float bwHalf = 0.5f * std::max(1.0f, safeBw);
  block.tuneNorm = clampf(tuning.tuneSmoothedHz / bwHalf, -1.0f, 1.0f);

  if (std::fabs(tuning.tuneSmoothedHz - tuning.tuneAppliedHz) >
          tuning.updateEps ||
      std::fabs(safeBw - tuning.bwAppliedHz) > tuning.updateEps) {
    publishTuningConfig(radio, tuning.tuneSmoothedHz, safeBw);
  }
}
