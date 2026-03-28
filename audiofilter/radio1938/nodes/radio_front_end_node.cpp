#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cmath>

namespace {

float resonantCapacitanceFarads(float freqHz, float inductanceHenries) {
  float omega = kRadioTwoPi * std::max(freqHz, 1.0f);
  return 1.0f / std::max(omega * omega * std::max(inductanceHenries, 1e-9f),
                         1e-18f);
}

float seriesResistanceForBandwidth(float inductanceHenries, float bandwidthHz) {
  return std::max(kRadioTwoPi * std::max(inductanceHenries, 1e-9f) *
                      std::max(bandwidthHz, 1.0f),
                  1e-4f);
}

void ensureFrontEndConfigured(Radio1938& radio) {
  auto& tuning = radio.tuning;
  auto& frontEnd = radio.frontEnd;
  if (frontEnd.appliedConfigRevision == tuning.configRevision) return;

  float safeAudioBw = std::max(tuning.tunedBw, 1.0f);
  float physicalChannelBw = 2.0f * safeAudioBw;
  // Two tuned RF circuits in cascade should not each be set to the final
  // desired channel width, or the combined response collapses the sidebands.
  // Derive broader per-stage RF bandwidths so the overall front-end still
  // passes the published AM channel while the IF strip remains the dominant
  // selectivity element.
  constexpr float kRfStageCascadeCompensation = 2.35f;
  // The pre/post biquad bandpasses are numerical helpers around the explicit RF
  // tanks; they must stay wider than the physical channel so they do not become
  // hidden selectivity bottlenecks.
  constexpr float kRfHelperBandpassCompensation = 5.0f;
  float preBw = std::max(physicalChannelBw * tuning.preBwScale *
                             kRfStageCascadeCompensation,
                         1.0f);
  float rfBw = std::max(physicalChannelBw * tuning.postBwScale *
                            kRfStageCascadeCompensation,
                        1.0f);
  float preHelperBw =
      std::max(physicalChannelBw * kRfHelperBandpassCompensation, preBw);
  float postHelperBw =
      std::max(physicalChannelBw * kRfHelperBandpassCompensation, rfBw);
  float rfCenterHz = tuning.sourceCarrierHz;
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
  frontEnd.antennaTank.setModel(
      radio.sampleRate, antennaInductance,
      frontEnd.antennaCapacitanceFarads,
      frontEnd.antennaSeriesResistanceOhms + antennaLoadResistance,
      antennaLoadResistance, 8);
  frontEnd.rfTank.setModel(radio.sampleRate, rfInductance,
                           frontEnd.rfCapacitanceFarads,
                           frontEnd.rfSeriesResistanceOhms + rfLoadResistance,
                           rfLoadResistance, 8);
  frontEnd.preLpfIn.setBandpass(
      radio.sampleRate, rfCenterHz,
      std::max(0.35f, rfCenterHz / std::max(preHelperBw, 1.0f)));
  frontEnd.preLpfOut.setBandpass(
      radio.sampleRate, rfCenterHz,
      std::max(0.35f, rfCenterHz / std::max(postHelperBw, 1.0f)));
  frontEnd.appliedConfigRevision = tuning.configRevision;
}

}  // namespace

void RadioFrontEndNode::init(Radio1938& radio, RadioInitContext&) {
  radio.frontEnd.hpf.setHighpass(radio.sampleRate, radio.frontEnd.inputHpHz,
                                 kRadioBiquadQ);
  radio.frontEnd.selectivityPeak.setPeaking(radio.sampleRate,
                                            radio.frontEnd.selectivityPeakHz,
                                            radio.frontEnd.selectivityPeakQ,
                                            radio.frontEnd.selectivityPeakGainDb);
  ensureFrontEndConfigured(radio);
}

void RadioFrontEndNode::reset(Radio1938& radio) {
  auto& frontEnd = radio.frontEnd;
  frontEnd.hpf.reset();
  frontEnd.preLpfIn.reset();
  frontEnd.preLpfOut.reset();
  frontEnd.selectivityPeak.reset();
  frontEnd.antennaTank.clearState();
  frontEnd.rfTank.clearState();
}

float RadioFrontEndNode::run(Radio1938& radio, float x, RadioSampleContext& ctx) {
  auto& frontEnd = radio.frontEnd;
  ensureFrontEndConfigured(radio);
  float rfHold = clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  float y = frontEnd.hpf.process(x);
  if (ctx.signal.mode == SourceInputMode::RealRf) {
    y = frontEnd.preLpfIn.process(y);
  }
  y = frontEnd.antennaTank.step(y);
  y *= frontEnd.rfGain * std::max(0.35f, 1.0f - frontEnd.avcGainDepth * rfHold);
  y = frontEnd.rfTank.step(y);
  if (ctx.signal.mode == SourceInputMode::RealRf) {
    y = frontEnd.preLpfOut.process(y);
  }
  y = frontEnd.selectivityPeak.process(y);
  return y;
}
