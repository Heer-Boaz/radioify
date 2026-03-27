#include "../radio.h"
#include "math/radio_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

float RadioTuningNode::applyFilters(Radio1938& radio, float tuneHz, float bwHz) {
  auto& tuning = radio.tuning;
  auto& frontEnd = radio.frontEnd;
  float safeAudioBw = bwHz;
  float physicalChannelBw = 2.0f * safeAudioBw;
  float preBw = physicalChannelBw * tuning.preBwScale;
  float rfBw = physicalChannelBw * tuning.postBwScale;
  RadioIFStripNode::setBandwidth(radio, safeAudioBw, tuneHz);
  float rfCenterHz = radio.ifStrip.sourceCarrierHz;
  auto resonantCapacitanceFarads = [](float freqHz, float inductanceHenries) {
    float omega = kRadioTwoPi * std::max(freqHz, 1.0f);
    return 1.0f / std::max(omega * omega * std::max(inductanceHenries, 1e-9f),
                           1e-18f);
  };
  auto seriesResistanceForBandwidth = [](float inductanceHenries,
                                         float bandwidthHz) {
    return std::max(kRadioTwoPi * std::max(inductanceHenries, 1e-9f) *
                        std::max(bandwidthHz, 1.0f),
                    1e-4f);
  };
  float antennaDrift = std::max(radio.identity.frontEndAntennaDrift, 0.5f);
  float rfDrift = std::max(radio.identity.frontEndRfDrift, 0.5f);
  float antennaInductance = frontEnd.antennaInductanceHenries * antennaDrift;
  float rfInductance = frontEnd.rfInductanceHenries * rfDrift;
  float antennaLoadResistance =
      frontEnd.antennaLoadResistanceOhms * (2.0f - antennaDrift);
  float rfLoadResistance = frontEnd.rfLoadResistanceOhms * (2.0f - rfDrift);

  frontEnd.antennaCapacitanceFarads =
      resonantCapacitanceFarads(rfCenterHz, antennaInductance);
  frontEnd.rfCapacitanceFarads =
      resonantCapacitanceFarads(rfCenterHz, rfInductance);
  frontEnd.antennaSeriesResistanceOhms =
      seriesResistanceForBandwidth(antennaInductance, preBw);
  frontEnd.rfSeriesResistanceOhms =
      seriesResistanceForBandwidth(rfInductance, rfBw);
  frontEnd.antennaTank.configure(
      radio.sampleRate, antennaInductance,
      frontEnd.antennaCapacitanceFarads,
      frontEnd.antennaSeriesResistanceOhms + antennaLoadResistance,
      antennaLoadResistance, 8);
  frontEnd.rfTank.configure(radio.sampleRate, rfInductance,
                            frontEnd.rfCapacitanceFarads,
                            frontEnd.rfSeriesResistanceOhms + rfLoadResistance,
                            rfLoadResistance, 8);

  frontEnd.preLpfIn.setBandpass(
      radio.sampleRate, rfCenterHz, std::max(0.35f, rfCenterHz / preBw));
  frontEnd.preLpfOut.setBandpass(
      radio.sampleRate, rfCenterHz, std::max(0.35f, rfCenterHz / rfBw));

  tuning.tunedBw = safeAudioBw;
  return safeAudioBw;
}

void RadioTuningNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& tuning = radio.tuning;
  tuning.afcCorrectionHz = 0.0f;
  initCtx.tunedBw = applyFilters(radio, tuning.tuneOffsetHz, radio.bwHz);
  tuning.tuneAppliedHz = tuning.tuneOffsetHz;
  tuning.bwAppliedHz = radio.bwHz;
  tuning.tuneSmoothedHz = tuning.tuneOffsetHz;
  tuning.bwSmoothedHz = radio.bwHz;
}

void RadioTuningNode::reset(Radio1938& radio) {
  auto& tuning = radio.tuning;
  tuning.afcCorrectionHz = 0.0f;
  tuning.tuneAppliedHz = tuning.tuneOffsetHz;
  tuning.bwAppliedHz = radio.bwHz;
  tuning.tuneSmoothedHz = tuning.tuneOffsetHz;
  tuning.bwSmoothedHz = radio.bwHz;
}

void RadioTuningNode::prepare(Radio1938& radio,
                              RadioBlockControl& block,
                              uint32_t frames) {
  auto& tuning = radio.tuning;
  auto& demod = radio.demod;
  auto& noiseRuntime = radio.noiseRuntime;
  float rate = std::max(1.0f, radio.sampleRate);
  float tick =
      1.0f - std::exp(-static_cast<float>(frames) / (rate * tuning.smoothTau));
  float effectiveTuneHz = tuning.tuneOffsetHz;
  if (tuning.magneticTuningEnabled) {
    effectiveTuneHz += tuning.afcCorrectionHz;
  }
  tuning.tuneSmoothedHz += tick * (effectiveTuneHz - tuning.tuneSmoothedHz);
  tuning.bwSmoothedHz += tick * (radio.bwHz - tuning.bwSmoothedHz);

  float safeBw = tuning.bwSmoothedHz;
  float bwHalf = 0.5f * std::max(1.0f, safeBw);
  block.tuneNorm = clampf(tuning.tuneSmoothedHz / bwHalf, -1.0f, 1.0f);

  if (std::fabs(tuning.tuneSmoothedHz - tuning.tuneAppliedHz) > tuning.updateEps ||
      std::fabs(tuning.bwSmoothedHz - tuning.bwAppliedHz) > tuning.updateEps) {
    float tunedBw = applyFilters(radio, tuning.tuneSmoothedHz, tuning.bwSmoothedHz);
    tuning.tuneAppliedHz = tuning.tuneSmoothedHz;
    tuning.bwAppliedHz = tuning.bwSmoothedHz;
    demod.am.setBandwidth(tunedBw, tuning.tuneSmoothedHz);
    noiseRuntime.hum.setFs(radio.sampleRate, tunedBw);
  }
}
