#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

std::array<float, 2> rotateComplexEnvelope(float inI,
                                           float inQ,
                                           float c,
                                           float s) {
  return {inI * c + inQ * s, inQ * c - inI * s};
}

std::array<float, 2> unrotateComplexEnvelope(float inI,
                                             float inQ,
                                             float c,
                                             float s) {
  return {inI * c - inQ * s, inQ * c + inI * s};
}

float resonantCapacitanceFarads(float freqHz, float inductanceHenries) {
  float omega = kRadioTwoPi * std::max(freqHz, 1.0f);
  return 1.0f /
         std::max(omega * omega * std::max(inductanceHenries, 1e-9f), 1e-18f);
}

float seriesResistanceForBandwidth(float inductanceHenries, float bandwidthHz) {
  return std::max(kRadioTwoPi * std::max(inductanceHenries, 1e-9f) *
                      std::max(bandwidthHz, 1.0f),
                  1e-4f);
}

float seriesBandwidthHz(float inductanceHenries, float resistanceOhms) {
  return std::max(resistanceOhms, 1e-4f) /
         (kRadioTwoPi * std::max(inductanceHenries, 1e-9f));
}

float parallelLoadBandwidthHz(float capacitanceFarads, float resistanceOhms) {
  if (capacitanceFarads <= 0.0f || resistanceOhms <= 0.0f) return 0.0f;
  return 1.0f / (kRadioTwoPi * resistanceOhms * capacitanceFarads);
}

float downmixImageHz(float sampleRate, float carrierHz) {
  return std::fabs(sampleRate - 2.0f * carrierHz);
}

void resetIfStripRuntimeEnvelopeState(Radio1938::IFStripNodeState& ifStrip) {
  ifStrip.sourceDownmixPhase = 0.0f;
  ifStrip.sourceDownmixOscCos = 1.0f;
  ifStrip.sourceDownmixOscSin = 0.0f;
  ifStrip.ifEnvelopePhase = 0.0f;
  ifStrip.ifEnvelopeOscCos = 1.0f;
  ifStrip.ifEnvelopeOscSin = 0.0f;
  ifStrip.sourceEnvelope.reset();
  ifStrip.loadedCanEnvelope.reset();
}

float computeIfTuneOffsetHz(const Radio1938::IFStripNodeState& ifStrip) {
  return ifStrip.loFrequencyHz - ifStrip.sourceCarrierHz - ifStrip.ifCenterHz;
}

std::array<float, 2> extractSourceEnvelope(Radio1938& radio,
                                           float rfSample,
                                           const RadioSignalFrame& signal,
                                           float rfGain) {
  auto& ifStrip = radio.ifStrip;
  if (signal.mode == SourceInputMode::ComplexEnvelope) {
    return {rfGain * signal.i, rfGain * signal.q};
  }

  float c = ifStrip.sourceDownmixOscCos;
  float s = ifStrip.sourceDownmixOscSin;
  auto sourceEnv = ifStrip.sourceEnvelope.process(2.0f * rfSample * rfGain * c,
                                                  -2.0f * rfSample * rfGain * s);
  advanceNormalizedOscillator(ifStrip.sourceDownmixStepCos,
                              ifStrip.sourceDownmixStepSin,
                              ifStrip.sourceDownmixOscCos,
                              ifStrip.sourceDownmixOscSin);
  ifStrip.sourceDownmixPhase =
      wrapPhase(ifStrip.sourceDownmixPhase + ifStrip.sourceDownmixStepRadians);
  return sourceEnv;
}

std::array<float, 2> runLoadedIfCanEquivalent(Radio1938& radio,
                                              float sourceEnvI,
                                              float sourceEnvQ,
                                              float mixerConversionGain,
                                              float ifGain) {
  auto& ifStrip = radio.ifStrip;
  float tuneCos = ifStrip.ifEnvelopeOscCos;
  float tuneSin = ifStrip.ifEnvelopeOscSin;
  auto rotatedEnv =
      rotateComplexEnvelope(sourceEnvI, sourceEnvQ, tuneCos, tuneSin);
  advanceNormalizedOscillator(ifStrip.ifEnvelopeStepCos,
                              ifStrip.ifEnvelopeStepSin,
                              ifStrip.ifEnvelopeOscCos,
                              ifStrip.ifEnvelopeOscSin);
  ifStrip.ifEnvelopePhase =
      wrapPhase(ifStrip.ifEnvelopePhase + ifStrip.ifEnvelopeStepRadians);

  float ifEnvI = rotatedEnv[0] * mixerConversionGain * ifGain;
  float ifEnvQ = rotatedEnv[1] * mixerConversionGain * ifGain;
  auto loadedCanEnv = ifStrip.loadedCanEnvelope.process(ifEnvI, ifEnvQ);

  // The detector must see the loaded IF can mapped back into the original
  // source-envelope frame. Unrotate only the residual tuning offset here;
  // source carrier removal was already handled by sourceDownmixPhase.
  return unrotateComplexEnvelope(loadedCanEnv[0], loadedCanEnv[1], tuneCos,
                                 tuneSin);
}

void configureIfStrip(Radio1938& radio) {
  auto& ifStrip = radio.ifStrip;
  const auto& tuning = radio.tuning;

  float sampleRate = std::max(radio.sampleRate, 1.0f);
  float safeAudioBw = std::max(tuning.tunedBw, ifStrip.ifMinBwHz);
  float physicalChannelBw = 2.0f * safeAudioBw;
  ifStrip.sourceCarrierHz = tuning.sourceCarrierHz;
  ifStrip.loFrequencyHz =
      ifStrip.sourceCarrierHz + ifStrip.ifCenterHz + tuning.tuneAppliedHz;
  ifStrip.sourceDownmixStepRadians =
      kRadioTwoPi * (ifStrip.sourceCarrierHz / sampleRate);
  ifStrip.sourceDownmixStepCos = std::cos(ifStrip.sourceDownmixStepRadians);
  ifStrip.sourceDownmixStepSin = std::sin(ifStrip.sourceDownmixStepRadians);
  ifStrip.ifEnvelopeStepRadians =
      kRadioTwoPi * (computeIfTuneOffsetHz(ifStrip) / sampleRate);
  ifStrip.ifEnvelopeStepCos = std::cos(ifStrip.ifEnvelopeStepRadians);
  ifStrip.ifEnvelopeStepSin = std::sin(ifStrip.ifEnvelopeStepRadians);

  float primaryDrift = std::max(radio.identity.ifPrimaryDrift, 0.65f);
  float secondaryDrift = std::max(radio.identity.ifSecondaryDrift, 0.65f);
  float couplingDrift = std::max(radio.identity.ifCouplingDrift, 0.65f);
  float primaryInductance =
      std::max(ifStrip.primaryInductanceHenries * primaryDrift, 1e-9f);
  float secondaryInductance =
      std::max(ifStrip.secondaryInductanceHenries * secondaryDrift, 1e-9f);
  float interstageCouplingCoeff =
      clampf(ifStrip.interstageCouplingCoeff * couplingDrift, 0.05f, 0.35f);
  float outputCouplingCoeff =
      clampf(ifStrip.outputCouplingCoeff * couplingDrift, 0.04f, 0.30f);
  float nominalCanBandwidthHz = std::max(physicalChannelBw * 1.10f, 1200.0f);
  float secondaryCapacitanceFarads =
      resonantCapacitanceFarads(ifStrip.ifCenterHz, secondaryInductance);
  float primarySeriesResistance =
      seriesResistanceForBandwidth(primaryInductance, nominalCanBandwidthHz);
  float secondarySeriesResistance =
      seriesResistanceForBandwidth(secondaryInductance, nominalCanBandwidthHz);
  float primaryTankBandwidthHz =
      seriesBandwidthHz(primaryInductance, primarySeriesResistance);
  float secondaryTankBandwidthHz =
      seriesBandwidthHz(secondaryInductance, secondarySeriesResistance);
  float secondaryLoadBandwidthHz = parallelLoadBandwidthHz(
      secondaryCapacitanceFarads, ifStrip.secondaryLoadResistanceOhms);
  float inductanceAsymmetry =
      std::clamp(std::sqrt(primaryInductance / secondaryInductance), 0.85f,
                 1.18f);
  float tankBandwidthTracking =
      std::clamp(0.5f * (primaryTankBandwidthHz + secondaryTankBandwidthHz) /
                     std::max(nominalCanBandwidthHz, 1.0f),
                 0.70f, 1.35f);
  float couplingMean = 0.5f * (interstageCouplingCoeff + outputCouplingCoeff);
  float loadSeverity =
      std::clamp(nominalCanBandwidthHz /
                     std::max(secondaryLoadBandwidthHz, nominalCanBandwidthHz),
                 0.0f, 1.0f);
  float tunedCanEnvelopeBandwidthHz =
      std::clamp(nominalCanBandwidthHz * tankBandwidthTracking *
                     (1.0f + 1.35f * couplingMean) *
                     (1.0f - 0.22f * std::fabs(inductanceAsymmetry - 1.0f)) *
                     (1.0f - 0.18f * loadSeverity),
                 std::max(1800.0f, 0.80f * safeAudioBw), 0.20f * sampleRate);
  float tunedCanEnvelopeQ =
      std::clamp(0.82f + 0.85f * couplingMean - 0.30f * loadSeverity, 0.70f,
                 1.10f);
  // Real-RF envelope extraction must reject the image at |fs - 2*fc| after
  // quadrature downmix. If this lowpass is tied to carrier frequency instead
  // of service bandwidth, the image leaks into the complex envelope and shows
  // up as a false audio whistle on an otherwise unmodulated carrier.
  float sourceEnvelopeMinHz =
      std::max(1.20f * safeAudioBw, 0.90f * tunedCanEnvelopeBandwidthHz);
  float sourceEnvelopeMaxHz =
      std::min(0.24f * sampleRate,
               0.60f * downmixImageHz(sampleRate, ifStrip.sourceCarrierHz));
  float sourceEnvelopeLpHz = std::clamp(
      sourceEnvelopeMinHz, std::max(sourceEnvelopeMinHz, 1800.0f),
      std::max(sourceEnvelopeMinHz, sourceEnvelopeMaxHz));

  ifStrip.sourceEnvelope.setLowpass(sampleRate, sourceEnvelopeLpHz,
                                    kRadioBiquadQ);
  ifStrip.loadedCanEnvelope.setLowpass(sampleRate, tunedCanEnvelopeBandwidthHz,
                                       tunedCanEnvelopeQ);
  ifStrip.senseLowHz = ifStrip.ifCenterHz - 0.5f * physicalChannelBw;
  ifStrip.senseHighHz = ifStrip.ifCenterHz + 0.5f * physicalChannelBw;
  ifStrip.demodBandwidthHz = safeAudioBw;
  ifStrip.demodTuneOffsetHz = tuning.tuneAppliedHz;
  ifStrip.appliedConfigRevision = tuning.configRevision;
}

void ensureIfStripConfigured(Radio1938& radio) {
  if (radio.ifStrip.appliedConfigRevision != radio.tuning.configRevision) {
    configureIfStrip(radio);
  }
}

}  // namespace

void RadioIFStripNode::init(Radio1938& radio, RadioInitContext&) {
  configureIfStrip(radio);
}

void RadioIFStripNode::reset(Radio1938& radio) {
  auto& ifStrip = radio.ifStrip;
  ifStrip.prevSourceMode = SourceInputMode::ComplexEnvelope;
  resetIfStripRuntimeEnvelopeState(ifStrip);
}

float RadioIFStripNode::run(Radio1938& radio, float y, RadioSampleContext& ctx) {
  auto& frontEnd = radio.frontEnd;
  auto& mixer = radio.mixer;
  auto& ifStrip = radio.ifStrip;
  ensureIfStripConfigured(radio);
  SourceInputMode mode = ctx.signal.mode;
  ctx.signal.detectorInputI = 0.0f;
  ctx.signal.detectorInputQ = 0.0f;
  if (!ifStrip.enabled || radio.sampleRate <= 0.0f) {
    return y;
  }

  float avcT = clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  float rfGain =
      frontEnd.rfGain * std::max(0.35f, 1.0f - frontEnd.avcGainDepth * avcT);
  float ifGainControl =
      std::max(0.20f, 1.0f - ifStrip.avcGainDepth * avcT);
  float ifGain = ifStrip.stageGain * ifGainControl;
  if (ifStrip.prevSourceMode != mode) {
    resetIfStripRuntimeEnvelopeState(ifStrip);
  }

  float mixerConversionGain = mixer.conversionGain;
  assert(std::isfinite(mixerConversionGain) &&
         std::fabs(mixerConversionGain) >= 1e-6f);
  auto sourceEnv = extractSourceEnvelope(radio, y, ctx.signal, rfGain);
  auto detectorEnv = runLoadedIfCanEquivalent(radio, sourceEnv[0], sourceEnv[1],
                                              mixerConversionGain, ifGain);
  assert(std::isfinite(detectorEnv[0]) && std::isfinite(detectorEnv[1]));
  ctx.signal.detectorInputI = detectorEnv[0];
  ctx.signal.detectorInputQ = detectorEnv[1];
  ctx.signal.setComplexEnvelope(detectorEnv[0], detectorEnv[1]);
  ifStrip.prevSourceMode = mode;

  if (radio.calibration.enabled) {
    float detectorMag =
        std::sqrt(detectorEnv[0] * detectorEnv[0] +
                  detectorEnv[1] * detectorEnv[1]);
    radio.calibration.detectorNodeVolts.accumulate(detectorMag);
  }

  return std::sqrt(detectorEnv[0] * detectorEnv[0] +
                   detectorEnv[1] * detectorEnv[1]);
}
