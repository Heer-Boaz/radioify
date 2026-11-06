#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
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

void configureFrontEnd(Radio1938& radio) {
  auto& tuning = radio.tuning;
  auto& frontEnd = radio.frontEnd;
  const double sampleRate = std::max(radio.sampleRate, 1.0f);

  // The tuned circuits are ganged with the dial, not the transmitted carrier.
  // Philco Magnetic Tuning pulls only the oscillator (Rider, 1937), so the
  // circuits stay at the knob position while the AFC corrects the IF.
  const double dialHz = tuning.stationFrequencyHz + tuning.dialAppliedHz;
  const double dialStep = 6.28318530717958647692 * tuning.dialAppliedHz / sampleRate;
  frontEnd.dialStepCos = static_cast<float>(std::cos(dialStep));
  frontEnd.dialStepSin = static_cast<float>(std::sin(dialStep));
  const double sourceStep =
      6.28318530717958647692 * tuning.sourceCarrierHz / sampleRate;
  frontEnd.sourceDownmixStepCos = static_cast<float>(std::cos(sourceStep));
  frontEnd.sourceDownmixStepSin = static_cast<float>(std::sin(sourceStep));

  // Trimmers keep each specimen aligned; its drift changes coil loss only.
  frontEnd.antennaTank.configure(
      sampleRate, dialHz, frontEnd.antennaInductanceHenries,
      frontEnd.antennaCoilQ * radio.identity.frontEndAntennaDrift,
      frontEnd.antennaShuntLoadOhms);
  frontEnd.tunedHz = dialHz;
  frontEnd.antennaLoadedQ = static_cast<float>(frontEnd.antennaTank.loadedQ());
  frontEnd.rfLoadedQ = 0.0f;
  if (frontEnd.secondTunedCircuitEnabled) {
    frontEnd.rfTank.configure(sampleRate, dialHz, frontEnd.rfInductanceHenries,
                              frontEnd.rfCoilQ * radio.identity.frontEndRfDrift,
                              frontEnd.rfShuntLoadOhms);
    frontEnd.rfLoadedQ = static_cast<float>(frontEnd.rfTank.loadedQ());
  }
  frontEnd.appliedConfigRevision = tuning.configRevision;
}

void ensureFrontEndConfigured(Radio1938& radio) {
  if (radio.frontEnd.appliedConfigRevision != radio.tuning.configRevision) {
    configureFrontEnd(radio);
  }
}

}  // namespace

std::array<float, 2> convertRealRfToSourceEnvelope(Radio1938& radio, float rf) {
  auto& frontEnd = radio.frontEnd;
  const float c = frontEnd.sourceDownmixOscCos;
  const float s = frontEnd.sourceDownmixOscSin;
  const auto envelope = frontEnd.sourceEnvelope.process(2.0f * rf * c, -2.0f * rf * s);
  advanceNormalizedOscillator(frontEnd.sourceDownmixStepCos,
                              frontEnd.sourceDownmixStepSin,
                              frontEnd.sourceDownmixOscCos,
                              frontEnd.sourceDownmixOscSin);
  return envelope;
}

void RadioFrontEndNode::init(Radio1938& radio, RadioInitContext&) {
  // Representation conversion is fixed for this sample rate. Keep the FIR
  // design (including Bessel evaluations) out of ongoing AFC retuning.
  const float sourceBw = radio.tuning.sourceBandwidthHz;
  const float carrierHz = radio.tuning.sourceCarrierHz;
  const float imageHz = std::min(2 * carrierHz, radio.sampleRate - 2 * carrierHz);
  radio.frontEnd.sourceEnvelope.configure(sourceBw / radio.sampleRate,
                                          (imageHz - sourceBw) / radio.sampleRate);
  configureFrontEnd(radio);
}

void RadioFrontEndNode::reset(Radio1938& radio) {
  auto& frontEnd = radio.frontEnd;
  frontEnd.sourceEnvelope.reset();
  frontEnd.sourceDownmixOscCos = 1.0f;
  frontEnd.sourceDownmixOscSin = 0.0f;
  frontEnd.dialOscCos = 1.0f;
  frontEnd.dialOscSin = 0.0f;
  frontEnd.antennaTank.reset();
  frontEnd.rfTank.reset();
}

float RadioFrontEndNode::run(Radio1938& radio, float x, RadioSampleContext& ctx) {
  auto& frontEnd = radio.frontEnd;
  ensureFrontEndConfigured(radio);
  auto envelope = ctx.signal.mode == SourceInputMode::RealRf
                      ? convertRealRfToSourceEnvelope(radio, x)
                      : std::array<float, 2>{ctx.signal.i, ctx.signal.q};

  const float c = frontEnd.dialOscCos;
  const float s = frontEnd.dialOscSin;
  envelope = rotateComplexEnvelope(envelope[0], envelope[1], c, s);
  envelope = frontEnd.antennaTank.process(envelope[0], envelope[1]);
  if (frontEnd.secondTunedCircuitEnabled) {
    envelope = frontEnd.rfTank.process(envelope[0], envelope[1]);
  }
  envelope = unrotateComplexEnvelope(envelope[0], envelope[1], c, s);
  advanceNormalizedOscillator(frontEnd.dialStepCos, frontEnd.dialStepSin,
                              frontEnd.dialOscCos, frontEnd.dialOscSin);

  const float rfHold = clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  const float gain =
      frontEnd.rfGain * std::max(0.35f, 1.0f - frontEnd.avcGainDepth * rfHold);
  ctx.signal.setComplexEnvelope(gain * envelope[0], gain * envelope[1]);
  return std::sqrt(ctx.signal.i * ctx.signal.i + ctx.signal.q * ctx.signal.q);
}
