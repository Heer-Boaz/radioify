#include "../radio.h"
#include "math/radio_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

static inline std::array<float, 2> rotateComplexEnvelope(float inI,
                                                         float inQ,
                                                         float phase) {
  float c = std::cos(phase);
  float s = std::sin(phase);
  return {inI * c + inQ * s, inQ * c - inI * s};
}

static inline std::array<float, 2> unrotateComplexEnvelope(float inI,
                                                           float inQ,
                                                           float phase) {
  float c = std::cos(phase);
  float s = std::sin(phase);
  return {inI * c - inQ * s, inQ * c + inI * s};
}

static float selectSourceCarrierHz(float outputFs,
                                   float internalFs,
                                   float ifCenterHz,
                                   float bwHz) {
  float maxCarrierByIf = 0.48f * internalFs - ifCenterHz - 8000.0f;
  float audioSidebandHz = 0.48f * std::max(bwHz, 1.0f);
  float maxCarrierByOutput =
      0.5f * std::max(outputFs, 1.0f) - audioSidebandHz - 1600.0f;
  float maxCarrier = maxCarrierByOutput;
  if (ifCenterHz > 0.0f) {
    maxCarrier = std::min(maxCarrierByIf, maxCarrierByOutput);
  }
  if (maxCarrier <= 6000.0f) {
    return std::clamp(0.25f * std::max(outputFs, 1.0f), 3000.0f,
                      std::max(3000.0f, maxCarrierByOutput));
  }
  return std::clamp(0.62f * maxCarrier, 6000.0f, maxCarrier);
}

void RadioIFStripNode::init(Radio1938& radio, RadioInitContext&) {
  setBandwidth(radio, radio.bwHz, radio.tuning.tuneOffsetHz);
}

void RadioIFStripNode::reset(Radio1938& radio) {
  auto& ifStrip = radio.ifStrip;
  ifStrip.sourceDownmixPhase = 0.0f;
  ifStrip.ifEnvelopePhase = 0.0f;
  ifStrip.detectorInputI = 0.0f;
  ifStrip.detectorInputQ = 0.0f;
  ifStrip.prevSourceMode = SourceInputMode::ComplexEnvelope;
  ifStrip.prevSourceI = 0.0f;
  ifStrip.prevSourceQ = 0.0f;
  ifStrip.sourceEnvelope.reset();
  ifStrip.loadedCanEnvelope.reset();
}

void RadioIFStripNode::setBandwidth(Radio1938& radio, float bwHz, float tuneHz) {
  auto& ifStrip = radio.ifStrip;
  auto& demod = radio.demod;
  auto resonantCapacitanceFarads = [](float freqHz, float inductanceHenries) {
    float omega = kRadioTwoPi * std::max(freqHz, 1.0f);
    return 1.0f /
           std::max(omega * omega * std::max(inductanceHenries, 1e-9f), 1e-18f);
  };
  auto seriesResistanceForBandwidth = [](float inductanceHenries,
                                         float bandwidthHz) {
    return std::max(kRadioTwoPi * std::max(inductanceHenries, 1e-9f) *
                        std::max(bandwidthHz, 1.0f),
                    1e-4f);
  };
  auto seriesBandwidthHz = [](float inductanceHenries, float resistanceOhms) {
    return std::max(resistanceOhms, 1e-4f) /
           (kRadioTwoPi * std::max(inductanceHenries, 1e-9f));
  };
  auto parallelLoadBandwidthHz = [](float capacitanceFarads,
                                    float resistanceOhms) {
    if (capacitanceFarads <= 0.0f || resistanceOhms <= 0.0f) return 0.0f;
    return 1.0f / (kRadioTwoPi * resistanceOhms * capacitanceFarads);
  };
  float sampleRate = std::max(radio.sampleRate, 1.0f);
  float safeAudioBw = std::max(bwHz, ifStrip.ifMinBwHz);
  float physicalChannelBw = 2.0f * safeAudioBw;
  ifStrip.sourceCarrierHz = selectSourceCarrierHz(
      sampleRate, sampleRate, 0.0f, physicalChannelBw);
  ifStrip.loFrequencyHz = ifStrip.sourceCarrierHz + ifStrip.ifCenterHz + tuneHz;

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
  float primaryCapacitanceFarads =
      resonantCapacitanceFarads(ifStrip.ifCenterHz, primaryInductance);
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
      std::clamp(std::sqrt(primaryInductance / secondaryInductance), 0.85f, 1.18f);
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
  // This stage is only the analytic downmix image rejector. It must stay
  // substantially wider than the loaded IF-can equivalent so the IF can, not a
  // cascaded helper LP, sets the audio sideband bandwidth.
  float sourceEnvelopeLpHz =
      std::clamp(std::max(4.0f * safeAudioBw, 0.72f * ifStrip.sourceCarrierHz),
                 2.0f * tunedCanEnvelopeBandwidthHz, 0.42f * sampleRate);
  // The IF strip stays a reduced-order baseband model, but its single complex
  // transfer is still derived from the tuned-can bandwidth, coupling, loading,
  // and primary/secondary mismatch. The user-facing bwHz remains an audio
  // sideband target; the physical IF channel that feeds this baseband transfer
  // is double-sideband around the carrier.
  ifStrip.sourceEnvelope.setLowpass(sampleRate, sourceEnvelopeLpHz,
                                    kRadioBiquadQ);
  ifStrip.loadedCanEnvelope.setLowpass(sampleRate, tunedCanEnvelopeBandwidthHz,
                                       tunedCanEnvelopeQ);

  float senseLow = ifStrip.ifCenterHz - 0.5f * physicalChannelBw;
  float senseHigh = ifStrip.ifCenterHz + 0.5f * physicalChannelBw;
  demod.am.setSenseWindow(senseLow, senseHigh);
  if (demod.am.fs > 0.0f) {
    demod.am.setBandwidth(safeAudioBw, tuneHz);
  }
}

float RadioIFStripNode::process(Radio1938& radio,
                                float y,
                                const RadioSampleContext& ctx) {
  auto& frontEnd = radio.frontEnd;
  auto& mixer = radio.mixer;
  auto& ifStrip = radio.ifStrip;
  ifStrip.detectorInputI = 0.0f;
  ifStrip.detectorInputQ = 0.0f;
  if (!ifStrip.enabled || radio.sampleRate <= 0.0f) {
    return y;
  }

  float frontEndT =
      clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  float rfGain =
      frontEnd.rfGain * std::max(0.35f, 1.0f - frontEnd.avcGainDepth * frontEndT);
  float ifGain =
      std::max(0.20f, 1.0f - ifStrip.avcGainDepth * frontEndT);
  float signal = y * rfGain * ifGain;

  if (ifStrip.prevSourceMode == SourceInputMode::RealRf &&
      radio.sourceFrame.mode == SourceInputMode::ComplexEnvelope) {
    ifStrip.sourceEnvelope.reset();
    ifStrip.loadedCanEnvelope.reset();
  }

  float tunePhase = ifStrip.sourceDownmixPhase;
  float tuneStep = kRadioTwoPi * (ifStrip.loFrequencyHz / radio.sampleRate);
  float sourceStep = kRadioTwoPi * (ifStrip.sourceCarrierHz / radio.sampleRate);
  float sourcePhase = ifStrip.ifEnvelopePhase;

  float sourceEnvI = signal * std::cos(sourcePhase);
  float sourceEnvQ = signal * std::sin(sourcePhase);
  auto rotatedEnv = rotateComplexEnvelope(sourceEnvI, sourceEnvQ, tunePhase);
  auto sourceEnv = ifStrip.sourceEnvelope.process(rotatedEnv[0], rotatedEnv[1]);
  ifStrip.sourceDownmixPhase = wrapPhase(sourcePhase + sourceStep);
  ifStrip.ifEnvelopePhase = wrapPhase(tunePhase + tuneStep);
  auto loadedCanEnv = ifStrip.loadedCanEnvelope.process(sourceEnv[0], sourceEnv[1]);
  auto detectorEnv =
      unrotateComplexEnvelope(loadedCanEnv[0], loadedCanEnv[1], tunePhase);
  assert(std::isfinite(detectorEnv[0]) && std::isfinite(detectorEnv[1]));
  ifStrip.detectorInputI = detectorEnv[0];
  ifStrip.detectorInputQ = detectorEnv[1];
  radio.sourceFrame.setComplexEnvelope(detectorEnv[0], detectorEnv[1]);
  ifStrip.prevSourceMode = radio.sourceFrame.mode;
  ifStrip.prevSourceI = detectorEnv[0];
  ifStrip.prevSourceQ = detectorEnv[1];

  if (radio.calibration.enabled) {
    float detectorMag =
        std::sqrt(detectorEnv[0] * detectorEnv[0] +
                  detectorEnv[1] * detectorEnv[1]);
    radio.calibration.detectorNodeVolts.accumulate(detectorMag);
  }

  return std::sqrt(detectorEnv[0] * detectorEnv[0] +
                   detectorEnv[1] * detectorEnv[1]);
}
