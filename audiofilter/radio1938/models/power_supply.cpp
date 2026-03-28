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

float computeRectifierRippleWave(const Radio1938::PowerNodeState& power) {
  float rectified = std::fabs(std::sin(power.rectifierPhase));
  float centered = rectified - (2.0f / kRadioPi);
  centered +=
      power.rippleSecondHarmonicMix * std::sin(2.0f * power.rectifierPhase);
  return centered;
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
    scale *= 1.0f + computeRectifierRippleWave(power) * rippleGain;
  }
  return clampPowerSupplyScale(power, scale);
}

void advanceRectifierRipplePhase(Radio1938& radio) {
  float rectifierHz = computeRectifierRippleHz(radio);
  if (rectifierHz <= 0.0f) return;
  radio.power.rectifierPhase = wrapPhase(
      radio.power.rectifierPhase +
      kRadioTwoPi * (rectifierHz / std::max(radio.sampleRate, 1.0f)));
}
