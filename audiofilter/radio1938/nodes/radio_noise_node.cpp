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
  hum.crackleHp.setHighpass(hum.fs, hum.noiseHpHz, hum.filterQ);
  hum.crackleLp.setLowpass(hum.fs, safeLp, hum.filterQ);
  hum.hp.reset();
  hum.lp.reset();
  hum.crackleHp.reset();
  hum.crackleLp.reset();
  hum.scAtk = std::exp(-1.0f / (hum.fs * (hum.scAttackMs / 1000.0f)));
  hum.scRel = std::exp(-1.0f / (hum.fs * (hum.scReleaseMs / 1000.0f)));
  hum.crackleDecay = std::exp(-1.0f / (hum.fs * (hum.crackleDecayMs / 1000.0f)));
}

void resetNoiseRuntime(NoiseHum& hum) {
  hum.humPhase = 0.0f;
  hum.scEnv = 0.0f;
  hum.crackleEnv = 0.0f;
  hum.cracklePulse = 0.0f;
  hum.pinkFast = 0.0f;
  hum.pinkSlow = 0.0f;
  hum.brown = 0.0f;
  hum.hissDrift = 0.0f;
  hum.hissDriftSlow = 0.0f;
  hum.hp.reset();
  hum.lp.reset();
  hum.crackleHp.reset();
  hum.crackleLp.reset();
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
  float programAbs = std::fabs(in.programSample);
  if (programAbs > hum.scEnv) {
    hum.scEnv = hum.scAtk * hum.scEnv + (1.0f - hum.scAtk) * programAbs;
  } else {
    hum.scEnv = hum.scRel * hum.scEnv + (1.0f - hum.scRel) * programAbs;
  }
  float maskT = clampf(hum.scEnv / hum.sidechainMaskRef, 0.0f, 1.0f);
  float hissMask = 1.0f - hum.hissMaskDepth * maskT;
  float burstMask = 1.0f - hum.burstMaskDepth * maskT;

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
  noise *= in.noiseAmp * hissMask;

  float crackle = 0.0f;
  if (in.crackleRate > 0.0f && in.crackleAmp > 0.0f && hum.fs > 0.0f) {
    float chance = in.crackleRate / hum.fs;
    if (hum.dist01(hum.rng) < chance) {
      float eventSign = (hum.dist(hum.rng) >= 0.0f) ? 1.0f : -1.0f;
      float eventAmp = 0.35f + 0.65f * hum.dist01(hum.rng);
      hum.crackleEnv = std::max(hum.crackleEnv, eventAmp);
      hum.cracklePulse = clampf(hum.cracklePulse + eventSign * eventAmp,
                                -1.5f, 1.5f);
    }
    hum.cracklePulse *= hum.crackleDecay;
    hum.crackleEnv *= hum.crackleDecay;
    float raw = hum.cracklePulse;
    raw = hum.crackleHp.process(raw);
    raw = hum.crackleLp.process(raw);
    crackle = raw * in.crackleAmp * burstMask;
  }

  float toneHum = 0.0f;
  if (in.humToneEnabled && in.humAmp > 0.0f && hum.fs > 0.0f) {
    hum.humPhase += kRadioTwoPi * (hum.humHz / hum.fs);
    if (hum.humPhase > kRadioTwoPi) hum.humPhase -= kRadioTwoPi;
    toneHum = std::sin(hum.humPhase) +
              hum.humSecondHarmonicMix * std::sin(2.0f * hum.humPhase);
    toneHum *= in.humAmp * hissMask;
  }

  return noise + crackle + toneHum;
}

}  // namespace

void RadioNoiseNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseRuntime = radio.noiseRuntime;
  configureNoiseRuntime(noiseRuntime.hum, radio.sampleRate, initCtx.tunedBw);
  noiseRuntime.hum.humHz = noiseConfig.humHzDefault;
}

void RadioNoiseNode::reset(Radio1938& radio) {
  resetNoiseRuntime(radio.noiseRuntime.hum);
}

float RadioNoiseNode::run(Radio1938& radio, float y, RadioSampleContext& ctx) {
  ensureNoiseRuntimeConfigured(radio);
  NoiseInput noiseIn{};
  noiseIn.programSample = y;
  noiseIn.noiseAmp = ctx.derived.noiseAmp;
  noiseIn.crackleAmp = ctx.derived.crackleAmp;
  noiseIn.crackleRate = ctx.derived.crackleRate;
  noiseIn.humAmp = ctx.derived.humAmp;
  noiseIn.humToneEnabled = ctx.derived.humToneEnabled;
  return y + processNoiseRuntime(radio.noiseRuntime.hum, noiseIn);
}
