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
  float avcT = clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  float rfGainControl = std::max(
      0.35f, 1.0f - radio.frontEnd.avcGainDepth * avcT);
  float ifGainControl = std::max(
      0.20f, 1.0f - radio.ifStrip.avcGainDepth * avcT);
  float preDetectorGain = rfGainControl * ifGainControl;
  float mistuneT = 0.0f;
  if (ctx.block) {
    mistuneT = clampf(std::fabs(ctx.block->tuneNorm), 0.0f, 1.0f);
  } else {
    float bwHalf = 0.5f * std::max(radio.tuning.tunedBw, 1.0f);
    mistuneT = clampf(std::fabs(radio.tuning.tuneAppliedHz) /
                          std::max(bwHalf, 1e-6f),
                      0.0f, 1.0f);
  }
  float detectorLockT =
      1.0f - clampf(std::fabs(radio.controlSense.tuningErrorSense),
                    0.0f, 1.0f);
  float tunedCaptureT = 1.0f - mistuneT;
  float carrierT = clampf(radio.controlBus.controlVoltage, 0.0f, 1.0f);
  float crackleExposure = 1.0f - clampf(carrierT * tunedCaptureT * detectorLockT,
                                        0.0f, 1.0f);
  float crackleExposureSq = crackleExposure * crackleExposure;

  // RF/IF interference follows the same AVC-governed gain as the incoming
  // carrier. Impulsive bursts are most audible when carrier capture is weak,
  // either because the station is weak or because the tuned passband is
  // offset enough that the detector sees an asymmetric envelope.
  ctx.derived.demodIfNoiseAmp =
      noiseDerived.baseNoiseAmp * radio.globals.ifNoiseMix * preDetectorGain;
  bool crackleAtDetector =
      radio.graph.isEnabled(PassId::IFStrip) &&
      radio.graph.isEnabled(PassId::Demod);
  if (crackleAtDetector) {
    ctx.derived.demodIfCrackleAmp =
        noiseDerived.baseCrackleAmp * preDetectorGain * crackleExposureSq;
    ctx.derived.demodIfCrackleRate =
        noiseDerived.crackleRate * crackleExposure;
    ctx.derived.crackleAmp = 0.0f;
    ctx.derived.crackleRate = 0.0f;
  } else {
    ctx.derived.demodIfCrackleAmp = 0.0f;
    ctx.derived.demodIfCrackleRate = 0.0f;
    ctx.derived.crackleAmp =
        noiseDerived.baseCrackleAmp * crackleExposureSq;
    ctx.derived.crackleRate =
        noiseDerived.crackleRate * crackleExposure;
  }
  ctx.derived.noiseAmp =
      noiseDerived.baseNoiseAmp * radio.globals.postNoiseMix;
  // Allow a small configurable hum contribution in addition to the supply
  // ripple path so preset-level hum controls are actually audible.
  ctx.derived.humAmp = noiseDerived.baseHumAmp;
  ctx.derived.humToneEnabled =
      radio.noiseConfig.enableHumTone && (noiseDerived.baseHumAmp > 0.0f);
}
