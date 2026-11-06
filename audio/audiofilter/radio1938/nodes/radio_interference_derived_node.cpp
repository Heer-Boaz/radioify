#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

void RadioInterferenceDerivedNode::init(Radio1938& radio, RadioInitContext&) {
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseDerived = radio.noiseDerived;
  noiseDerived.baseHumAmp =
      noiseConfig.enableHumTone ? std::max(noiseConfig.humAmpScale, 0.0f)
                                : 0.0f;
  if (radio.noiseWeight <= 0.0f) {
    noiseDerived.baseNoiseAmp = 0.0f;
    noiseDerived.baseCrackleAmp = 0.0f;
    noiseDerived.crackleRate = 0.0f;
    return;
  }

  float scale =
      radio.noiseWeight / std::max(noiseConfig.noiseWeightRef, 1e-6f);
  if (noiseConfig.noiseWeightScaleMax > 0.0f) {
    scale = std::min(scale, noiseConfig.noiseWeightScaleMax);
  }
  noiseDerived.baseNoiseAmp = radio.noiseWeight;
  noiseDerived.baseCrackleAmp = noiseConfig.crackleAmpScale * scale;
  noiseDerived.crackleRate = noiseConfig.crackleRateScale * scale;
}

void RadioInterferenceDerivedNode::reset(Radio1938&) {}

void RadioInterferenceDerivedNode::run(Radio1938& radio,
                                       RadioSampleContext& ctx) {
  auto& noiseDerived = radio.noiseDerived;
  // Source intensities do not depend on programme level, tuning or AVC.
  // Filtering and gain control act on the actual RF/noise signal downstream.
  ctx.derived.ifInputNoiseAmp = noiseDerived.baseNoiseAmp * radio.globals.ifNoiseMix;
  ctx.derived.rfImpulseAmp = noiseDerived.baseCrackleAmp;
  ctx.derived.rfImpulseRate = noiseDerived.crackleRate;
  ctx.derived.noiseAmp =
      noiseDerived.baseNoiseAmp * radio.globals.postNoiseMix;
  // Allow a small configurable hum contribution in addition to the supply
  // ripple path so preset-level hum controls are actually audible.
  ctx.derived.humAmp = noiseDerived.baseHumAmp;
  ctx.derived.humToneEnabled =
      radio.noiseConfig.enableHumTone && (noiseDerived.baseHumAmp > 0.0f);
}
