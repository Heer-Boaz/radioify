#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cmath>

namespace {

float clampPublishedBandwidth(const Radio1938& radio, float bwHz) {
  const auto& tuning = radio.tuning;
  float sampleRateLimitedBwHz =
      0.20f * std::max(radio.sampleRate, 1.0f);
  float safeBwMinHz = std::min(std::max(tuning.safeBwMinHz, 1.0f), sampleRateLimitedBwHz);
  float safeBwMaxHz =
      std::max(safeBwMinHz, std::min(tuning.safeBwMaxHz, sampleRateLimitedBwHz));
  return std::clamp(bwHz, safeBwMinHz, safeBwMaxHz);
}

float publishTuningConfig(Radio1938& radio, float tuneHz, float bwHz) {
  auto& tuning = radio.tuning;
  float safeAudioBw = clampPublishedBandwidth(radio, bwHz);
  // A fixed representation carrier leaves equal room for the two AM
  // sidebands. Transmitter bandwidth is independent of the tuned receiver.
  tuning.sourceBandwidthHz = std::min(10000.0f, .20f * radio.sampleRate);
  const float sourceCarrierHz = .25f * radio.sampleRate;
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
  const float rate = std::max(radio.sampleRate, 1.0f);
  tuning.smoothingTick = 1 - std::exp(-64 / (rate * std::max(tuning.smoothTau, 1e-4f)));
  const float afcSeconds = tuning.afcResponseMs * 0.001f;
  tuning.afcTick =
      (afcSeconds > 0.0f) ? 1.0f - std::exp(-1.0f / (rate * afcSeconds))
                          : 0.0f;
  tuning.tuneSmoothedHz = tuning.tuneOffsetHz;
  tuning.bwSmoothedHz = clampPublishedBandwidth(radio, radio.bwHz);
  initCtx.tunedBw =
      publishTuningConfig(radio, tuning.tuneSmoothedHz, tuning.bwSmoothedHz);
}

void RadioTuningNode::reset(Radio1938& radio) {
  auto& tuning = radio.tuning;
  tuning.afcCorrectionHz = 0.0f;
  tuning.tuneSmoothedHz = tuning.tuneOffsetHz;
  tuning.bwSmoothedHz = clampPublishedBandwidth(radio, radio.bwHz);
  publishTuningConfig(radio, tuning.tuneSmoothedHz, tuning.bwSmoothedHz);
  tuning.updateCountdown = 0;
}

void RadioTuningNode::run(Radio1938& radio, RadioSampleContext&) {
  auto& tuning = radio.tuning;
  if (tuning.updateCountdown > 0) {
    --tuning.updateCountdown;
    return;
  }
  tuning.updateCountdown = 63;
  const float tick = tuning.smoothingTick;
  float effectiveTuneHz = tuning.tuneOffsetHz;
  if (tuning.magneticTuningEnabled) {
    effectiveTuneHz += tuning.afcCorrectionHz;
  }
  float targetBwHz = clampPublishedBandwidth(radio, radio.bwHz);
  tuning.tuneSmoothedHz += tick * (effectiveTuneHz - tuning.tuneSmoothedHz);
  tuning.bwSmoothedHz += tick * (targetBwHz - tuning.bwSmoothedHz);

  float safeBw = clampPublishedBandwidth(radio, tuning.bwSmoothedHz);
  if (std::fabs(tuning.tuneSmoothedHz - tuning.tuneAppliedHz) >
          tuning.updateEps ||
      std::fabs(safeBw - tuning.bwAppliedHz) > tuning.updateEps) {
    publishTuningConfig(radio, tuning.tuneSmoothedHz, safeBw);
  }
}
