#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

void RadioFinalLimiterNode::init(Radio1938& radio, RadioInitContext&) {
  auto& limiter = radio.finalLimiter;
  limiter.attackCoeff =
      std::exp(-1.0f / (radio.sampleRate * (limiter.attackMs / 1000.0f)));
  limiter.releaseCoeff =
      std::exp(-1.0f / (radio.sampleRate * (limiter.releaseMs / 1000.0f)));
  limiter.delaySamples =
      static_cast<int>(std::lround(radio.sampleRate *
                                   (limiter.lookaheadMs / 1000.0f)));
  limiter.delayBuf.assign(static_cast<size_t>(limiter.delaySamples), 0.0f);
  limiter.requiredGainBuf.assign(static_cast<size_t>(limiter.delaySamples), 1.0f);
  limiter.delayWriteIndex = 0;
  limiter.peakOversampler.configure(radio.globals.oversampleFactor,
                                   radio.globals.oversampleCutoffFraction);
}

void RadioFinalLimiterNode::reset(Radio1938& radio) {
  auto& limiter = radio.finalLimiter;
  limiter.gain = 1.0f;
  limiter.targetGain = 1.0f;
  limiter.observedPeak = 0.0f;
  limiter.delayWriteIndex = 0;
  std::fill(limiter.delayBuf.begin(), limiter.delayBuf.end(), 0.0f);
  std::fill(limiter.requiredGainBuf.begin(), limiter.requiredGainBuf.end(), 1.0f);
  limiter.peakOversampler.reset();
}

float RadioFinalLimiterNode::run(Radio1938& radio,
                                 float y,
                                 RadioSampleContext&) {
  auto& limiter = radio.finalLimiter;
  if (!limiter.enabled) return y;
  float limitedIn = y;

  // The interpolator is delayed. Include the current discrete sample as well
  // so a short/disabled lookahead cannot miss an immediately arriving peak.
  float peak = std::fabs(limitedIn);
  const auto& interpolated = limiter.peakOversampler.upsample(limitedIn);
  for (int i = 0; i < limiter.peakOversampler.factor(); ++i)
    peak = std::max(peak, std::fabs(interpolated[i]));
  limiter.observedPeak = peak;

  float requiredGain = 1.0f;
  if (peak > limiter.threshold && peak > 1e-9f) {
    requiredGain = limiter.threshold / peak;
  }

  float delayed = limitedIn;
  if (!limiter.delayBuf.empty()) {
    size_t writeIndex = static_cast<size_t>(limiter.delayWriteIndex);
    delayed = limiter.delayBuf[writeIndex];
    limiter.delayBuf[writeIndex] = limitedIn;
    limiter.requiredGainBuf[writeIndex] = requiredGain;
    limiter.delayWriteIndex =
        (limiter.delayWriteIndex + 1) % static_cast<int>(limiter.delayBuf.size());
    limiter.targetGain = 1.0f;
    for (float gainCandidate : limiter.requiredGainBuf) {
      limiter.targetGain = std::min(limiter.targetGain, gainCandidate);
    }
  } else {
    limiter.targetGain = requiredGain;
  }

  if (limiter.targetGain < limiter.gain) {
    limiter.gain = limiter.targetGain;
  } else {
    limiter.gain =
        limiter.releaseCoeff * limiter.gain +
        (1.0f - limiter.releaseCoeff) * limiter.targetGain;
  }

  float out = delayed * limiter.gain;
  float gainReduction = 1.0f - limiter.gain;
  float gainReductionDb = (limiter.gain < 0.999999f) ? -lin2db(limiter.gain) : 0.0f;

  radio.diagnostics.finalLimiterPeak =
      std::max(radio.diagnostics.finalLimiterPeak, peak);
  radio.diagnostics.finalLimiterGain =
      std::min(radio.diagnostics.finalLimiterGain, limiter.gain);
  radio.diagnostics.finalLimiterMaxGainReduction =
      std::max(radio.diagnostics.finalLimiterMaxGainReduction, gainReduction);
  radio.diagnostics.finalLimiterMaxGainReductionDb =
      std::max(radio.diagnostics.finalLimiterMaxGainReductionDb, gainReductionDb);
  radio.diagnostics.finalLimiterGainReductionSum += gainReduction;
  radio.diagnostics.finalLimiterGainReductionDbSum += gainReductionDb;
  radio.diagnostics.processedSamples++;
  if (limiter.gain < 0.999f) {
    radio.diagnostics.finalLimiterActive = true;
    radio.diagnostics.finalLimiterActiveSamples++;
  }

  if (radio.calibration.enabled) {
    if (std::fabs(limitedIn) > radio.globals.outputClipThreshold) {
      radio.calibration.preLimiterClipCount++;
    }
    if (std::fabs(out) > radio.globals.outputClipThreshold) {
      radio.calibration.postLimiterClipCount++;
    }
    radio.calibration.totalSamples++;
    radio.calibration.limiterGainReductionSum += gainReduction;
    radio.calibration.limiterGainReductionDbSum += gainReductionDb;
    if (limiter.gain < 0.999f) {
      radio.calibration.limiterActiveSamples++;
    }
    radio.calibration.limiterMaxGainReduction =
        std::max(radio.calibration.limiterMaxGainReduction, gainReduction);
    radio.calibration.limiterMaxGainReductionDb =
        std::max(radio.calibration.limiterMaxGainReductionDb, gainReductionDb);
  }

  return out;
}
