#include "power_supply.h"

#include "../../math/signal_math.h"

#include <algorithm>
#include <cmath>

float computePowerLoadT(const Radio1938::PowerNodeState& power) {
  return clampf((power.sagEnv - power.sagStart) /
                    std::max(1e-6f, power.sagEnd - power.sagStart),
                0.0f, 1.0f);
}

namespace {

float computeRectifierRippleHz(const Radio1938& radio) {
  float mainsHz = std::max(radio.noiseConfig.humHzDefault, 0.0f);
  float rectifierHz = 2.0f * mainsHz;
  if (radio.power.rectifierMinHz > 0.0f) {
    rectifierHz = std::max(rectifierHz, radio.power.rectifierMinHz);
  }
  return rectifierHz;
}

float computeRectifierRippleWaveFromOsc(
    const Radio1938::PowerNodeState& power) {
  float rectified = std::fabs(power.rectifierOscSin);
  float centered = rectified - (2.0f / kRadioPi);
  centered += power.rippleSecondHarmonicMix * 2.0f * power.rectifierOscSin *
              power.rectifierOscCos;
  return centered;
}

void refreshRectifierRippleWave(Radio1938::PowerNodeState& power) {
  power.rectifierRippleWave = computeRectifierRippleWaveFromOsc(power);
}

float clampPowerSupplyScale(const Radio1938::PowerNodeState& power,
                            float scale) {
  float low = std::min(power.gainMin, power.gainMax);
  float high = std::max(power.gainMin, power.gainMax);
  if (high > 0.0f) {
    if (low <= 0.0f) low = 0.0f;
    scale = std::clamp(scale, low, high);
  }
  return std::max(scale, 0.0f);
}

}  // namespace

float computePowerBranchSupplyScale(const Radio1938& radio, float branchDepth) {
  const auto& power = radio.power;
  float powerT = computePowerLoadT(power);
  float scale = 1.0f - power.gainSagPerPower * powerT;
  branchDepth = std::max(branchDepth, 0.0f);
  if (power.rippleDepth > 0.0f && branchDepth > 0.0f) {
    float rippleGain =
        power.rippleDepth * branchDepth *
        (power.rippleGainBase + power.rippleGainDepth * powerT);
    scale *= 1.0f + power.rectifierRippleWave * rippleGain;
  }
  return clampPowerSupplyScale(power, scale);
}

void configureRectifierRipple(Radio1938& radio) {
  auto& power = radio.power;
  float rectifierHz = computeRectifierRippleHz(radio);
  float sampleRate = (radio.power.internalSampleRate > 0.0f)
                         ? radio.power.internalSampleRate
                         : radio.sampleRate;
  power.rectifierStepRadians =
      (rectifierHz > 0.0f)
          ? kRadioTwoPi * (rectifierHz / std::max(sampleRate, 1.0f))
          : 0.0f;
  power.rectifierStepCos = std::cos(power.rectifierStepRadians);
  power.rectifierStepSin = std::sin(power.rectifierStepRadians);
  refreshRectifierRippleWave(power);
}

void resetRectifierRipple(Radio1938& radio) {
  auto& power = radio.power;
  power.rectifierPhase = 0.0f;
  power.rectifierOscCos = 1.0f;
  power.rectifierOscSin = 0.0f;
  refreshRectifierRippleWave(power);
}

void advanceRectifierRipplePhase(Radio1938& radio) {
  auto& power = radio.power;
  if (power.rectifierStepRadians == 0.0f) return;
  advanceNormalizedOscillator(power.rectifierStepCos, power.rectifierStepSin,
                              power.rectifierOscCos, power.rectifierOscSin);
  power.rectifierPhase =
      wrapPhase(power.rectifierPhase + power.rectifierStepRadians);
  refreshRectifierRippleWave(power);
}
