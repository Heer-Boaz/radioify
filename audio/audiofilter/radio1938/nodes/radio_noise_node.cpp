#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

void configureNoiseRuntime(NoiseHum& hum, float sampleRate, float noiseBwHz) {
  hum.fs = sampleRate;
  hum.noiseLpHz = (noiseBwHz > 0.0f) ? noiseBwHz : hum.noiseLpHz;
  float safeLp = std::clamp(hum.noiseLpHz, hum.noiseHpHz + 200.0f, hum.fs * 0.45f);
  hum.hp.setHighpass(hum.fs, hum.noiseHpHz, hum.filterQ);
  hum.lp.setLowpass(hum.fs, safeLp, hum.filterQ);
  hum.hp.reset();
  hum.lp.reset();
  hum.humStepRadians = kRadioTwoPi * (hum.humHz / std::max(hum.fs, 1.0f));
  hum.humStepCos = std::cos(hum.humStepRadians);
  hum.humStepSin = std::sin(hum.humStepRadians);
}

void resetNoiseRuntime(NoiseHum& hum) {
  hum.humPhase = 0.0f;
  hum.humOscCos = 1.0f;
  hum.humOscSin = 0.0f;
  hum.pinkFast = 0.0f;
  hum.pinkSlow = 0.0f;
  hum.brown = 0.0f;
  hum.hissDrift = 0.0f;
  hum.hissDriftSlow = 0.0f;
  hum.hp.reset();
  hum.lp.reset();
}

void ensureNoiseRuntimeConfigured(Radio1938& radio) {
  auto& hum = radio.noiseRuntime.hum;
  float tunedBw = std::max(radio.tuning.tunedBw, 0.0f);
  bool sampleRateChanged = std::fabs(hum.fs - radio.sampleRate) > 1e-6f;
  bool bandwidthChanged = std::fabs(hum.noiseLpHz - tunedBw) > 1e-3f;
  if (sampleRateChanged || bandwidthChanged) {
    configureNoiseRuntime(hum, radio.sampleRate, tunedBw);
  }
}

float processNoiseRuntime(NoiseHum& hum, const NoiseInput& in) {
  float white = hum.dist(hum.rng);
  hum.pinkFast = hum.pinkFastPole * hum.pinkFast + (1.0f - hum.pinkFastPole) * white;
  hum.pinkSlow = hum.pinkSlowPole * hum.pinkSlow + (1.0f - hum.pinkSlowPole) * white;
  hum.brown = clampf(hum.brown + hum.brownStep * white, -1.0f, 1.0f);
  hum.hissDrift = hum.hissDriftPole * hum.hissDrift + hum.hissDriftNoise * hum.dist(hum.rng);
  hum.hissDriftSlow =
      hum.hissDriftSlowPole * hum.hissDriftSlow + hum.hissDriftSlowNoise * hum.dist(hum.rng);
  float noise = hum.whiteMix * white + hum.pinkFastMix * hum.pinkFast +
                hum.pinkDifferenceMix * (hum.pinkSlow - hum.pinkFastSubtract * hum.pinkFast) +
                hum.brownMix * hum.brown;
  noise *= hum.hissBase + hum.hissDriftDepth * hum.hissDrift;
  noise += hum.hissDriftSlowMix * hum.hissDriftSlow;
  noise = hum.hp.process(noise);
  noise = hum.lp.process(noise);
  noise *= in.noiseAmp;

  float toneHum = 0.0f;
  if (in.humToneEnabled && in.humAmp > 0.0f && hum.fs > 0.0f) {
    advanceNormalizedOscillator(hum.humStepCos, hum.humStepSin, hum.humOscCos,
                                hum.humOscSin);
    hum.humPhase = wrapPhase(hum.humPhase + hum.humStepRadians);
    const float s1 = hum.humOscSin, c1 = hum.humOscCos;
    const float s2 = 2 * s1 * c1, c2 = c1 * c1 - s1 * s1;
    // Full-wave rectifier residual: 2f, 4f, 6f. Separate f pickup allows
    // heater/ground leakage without confusing it with rectifier ripple.
    toneHum = hum.humMainsPickupMix * s1 + s2 + hum.humSecondHarmonicMix * 2 * s2 * c2 +
              hum.humThirdHarmonicMix * s2 * (3 - 4 * s2 * s2);
    toneHum *= in.humAmp;
  }

  return noise + toneHum;
}

}  // namespace

void RadioNoiseNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseRuntime = radio.noiseRuntime;
  noiseRuntime.hum.humHz = noiseConfig.humHzDefault;
  configureNoiseRuntime(noiseRuntime.hum, radio.sampleRate, initCtx.tunedBw);
  const auto& speaker = radio.speakerStage.speaker;
  const float mass = speaker.effectiveMovingMassKg;
  const float omega =
      1.0f / std::sqrt(mass * speaker.effectiveSuspensionComplianceMetersPerNewton);
  noiseRuntime.radiation.setHighpass(
      radio.sampleRate, omega / kRadioTwoPi,
      mass * omega / speaker.effectiveMechanicalDampingNsPerMeter);
}

void RadioNoiseNode::reset(Radio1938& radio) {
  resetNoiseRuntime(radio.noiseRuntime.hum);
  radio.noiseRuntime.radiation.reset();
}

float RadioNoiseNode::run(Radio1938& radio, float y, RadioSampleContext& ctx) {
  ensureNoiseRuntimeConfigured(radio);
  NoiseInput noiseIn{};
  noiseIn.noiseAmp = ctx.derived.noiseAmp;
  noiseIn.humAmp = ctx.derived.humAmp;
  noiseIn.humToneEnabled = ctx.derived.humToneEnabled;
  return y + radio.noiseRuntime.radiation.process(
                 processNoiseRuntime(radio.noiseRuntime.hum, noiseIn));
}
