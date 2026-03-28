#include "../../../radio.h"
#include "../../math/signal_math.h"
#include "../../models/tube_models.h"

#include <algorithm>
#include <cassert>
#include <cmath>

static float estimateMixerEnvelopeConversionGain(
    const FixedPlatePentodeEvaluator& plateCurrentForGrid,
    float mixedBaseGridVolts,
    float loGridDriveVolts) {
  constexpr int kMixerHarmonicSamples = 8;
  float baseDerivative = plateCurrentForGrid.evalDerivative(mixedBaseGridVolts);
  float harmonicSum = 0.0f;
  for (int i = 0; i < kMixerHarmonicSamples; ++i) {
    float phase =
        kRadioTwoPi * (static_cast<float>(i) + 0.5f) /
        static_cast<float>(kMixerHarmonicSamples);
    float loGridVolts = loGridDriveVolts * std::cos(phase);
    float drivenDerivative = plateCurrentForGrid.evalDerivative(
        mixedBaseGridVolts + loGridVolts);
    harmonicSum += (drivenDerivative - baseDerivative) * std::cos(phase);
  }
  return harmonicSum / static_cast<float>(kMixerHarmonicSamples);
}

void RadioMixerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& mixer = radio.mixer;
  float dcLoadResistanceOhms = 0.0f;
  if (mixer.plateCurrentAmps > 1e-6f &&
      mixer.plateSupplyVolts > mixer.plateDcVolts) {
    dcLoadResistanceOhms =
        (mixer.plateSupplyVolts - mixer.plateDcVolts) / mixer.plateCurrentAmps;
  }
  PentodeOperatingPoint op = solvePentodeOperatingPoint(
      mixer.plateSupplyVolts, mixer.screenVolts, dcLoadResistanceOhms,
      mixer.biasVolts, mixer.plateDcVolts, mixer.plateCurrentAmps,
      mixer.mutualConductanceSiemens, mixer.plateKneeVolts,
      mixer.gridSoftnessVolts, mixer.modelCutoffVolts);
  mixer.plateQuiescentVolts = op.plateVolts;
  mixer.plateQuiescentCurrentAmps = op.plateCurrentAmps;
  mixer.plateResistanceOhms = op.rpOhms;
  assert((std::fabs(mixer.plateQuiescentVolts - mixer.plateDcVolts) <=
          mixer.operatingPointToleranceVolts) &&
         "Mixer operating point diverged from the preset target");
}

void RadioMixerNode::reset(Radio1938& radio) {
  radio.mixer.mixedBaseGridVolts = 0.0f;
  radio.mixer.conversionGain = 1.0f;
  radio.mixer.inputDriveEnv = 0.0f;
}

float RadioMixerNode::run(Radio1938& radio, float y, RadioSampleContext& ctx) {
  auto& mixer = radio.mixer;
  float avcT = clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  float rfGridDrive =
      mixer.rfGridDriveVolts * std::max(radio.identity.mixerDriveDrift, 0.65f);
  float loGridBias =
      mixer.loGridBiasVolts * std::max(radio.identity.mixerBiasDrift, 0.65f);
  float baseGridVolts = mixer.biasVolts - mixer.avcGridDriveVolts * avcT;
  mixer.mixedBaseGridVolts = baseGridVolts + loGridBias;
  FixedPlatePentodeEvaluator mixerPlateCurrentForGrid =
      prepareFixedPlatePentodeEvaluator(
          mixer.plateQuiescentVolts, mixer.screenVolts, mixer.biasVolts,
          mixer.modelCutoffVolts, mixer.plateQuiescentVolts, mixer.screenVolts,
          mixer.plateQuiescentCurrentAmps, mixer.mutualConductanceSiemens,
          mixer.plateKneeVolts, mixer.gridSoftnessVolts);
  mixer.conversionGain =
      estimateMixerEnvelopeConversionGain(mixerPlateCurrentForGrid,
                                          mixer.mixedBaseGridVolts,
                                          mixer.loGridDriveVolts) *
      rfGridDrive * mixer.acLoadResistanceOhms;

  float signalMagnitude = std::fabs(y);
  if (ctx.signal.mode == SourceInputMode::ComplexEnvelope) {
    signalMagnitude = std::sqrt(ctx.signal.i * ctx.signal.i +
                                ctx.signal.q * ctx.signal.q);
  }
  float sampleRate = std::max(radio.sampleRate, 1.0f);
  float envCoeff = std::exp(-1.0f / (sampleRate * 0.0015f));
  mixer.inputDriveEnv =
      envCoeff * mixer.inputDriveEnv + (1.0f - envCoeff) * signalMagnitude;

  if (ctx.signal.mode == SourceInputMode::ComplexEnvelope) {
    return ctx.signal.i;
  }

  return y;
}
