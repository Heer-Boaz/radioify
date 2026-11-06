#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>

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

void resetIfStripRuntimeEnvelopeState(Radio1938::IFStripNodeState& ifStrip) {
  ifStrip.sourceDownmixPhase = 0.0f;
  ifStrip.sourceDownmixOscCos = 1.0f;
  ifStrip.sourceDownmixOscSin = 0.0f;
  ifStrip.ifEnvelopePhase = 0.0f;
  ifStrip.ifEnvelopeOscCos = 1.0f;
  ifStrip.ifEnvelopeOscSin = 0.0f;
  ifStrip.sourceEnvelope.reset();
  for (auto& transformer : ifStrip.transformers) transformer.reset();
  ifStrip.previousIfInput = {};
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
  // Real RF has already traversed FrontEnd, including its gain and AVC.
  // Applying rfGain here again would count that stage twice.
  const auto sourceEnv =
      ifStrip.sourceEnvelope.process(2.0f * rfSample * c, -2.0f * rfSample * s);
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
                                              float ifGain,
                                              float inputNoiseAmp) {
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

  float ifEnvI = rotatedEnv[0] * mixerConversionGain;
  float ifEnvQ = rotatedEnv[1] * mixerConversionGain;
  if (inputNoiseAmp > 0.0f) {
    // Independent quadratures model equivalent IF input noise. Preserve its
    // spectral density as the envelope sample rate changes; the IF filter
    // determines the integrated bandwidth. Absolute noise figure is unmeasured.
    constexpr float kInvSqrt2 = 0.70710678118f;
    const float scale = inputNoiseAmp * ifStrip.noiseSampleScale * kInvSqrt2;
    ifEnvI += ifStrip.noiseDist(ifStrip.noiseRng) * scale;
    ifEnvQ += ifStrip.noiseDist(ifStrip.noiseRng) * scale;
  }
  ifEnvI *= ifGain;
  ifEnvQ *= ifGain;
  std::array<float, 2> loadedCanEnv{};
  // Four short envelope steps limit trapezoidal frequency warping. All cans
  // share this clock; there is no intermediate host-rate down/up sampling.
  for (int step = 1; step <= 4; ++step) {
    const float t = .25f * step;
    loadedCanEnv = {
        ifStrip.previousIfInput[0] + t * (ifEnvI - ifStrip.previousIfInput[0]),
        ifStrip.previousIfInput[1] + t * (ifEnvQ - ifStrip.previousIfInput[1])};
    for (int can = 0; can < ifStrip.transformerCount; ++can)
      loadedCanEnv = ifStrip.transformers[can].process(loadedCanEnv[0], loadedCanEnv[1]);
  }
  ifStrip.previousIfInput = {ifEnvI, ifEnvQ};

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
  float safeAudioBw =
      std::min(std::max(tuning.tunedBw, ifStrip.ifMinBwHz), .20f * sampleRate);
  float physicalChannelBw = 2.0f * safeAudioBw;
  ifStrip.sourceCarrierHz = tuning.sourceCarrierHz;
  ifStrip.loFrequencyHz =
      ifStrip.sourceCarrierHz + ifStrip.ifCenterHz + tuning.tuneAppliedHz;
  ifStrip.sourceDownmixStepRadians =
      kRadioTwoPi * (ifStrip.sourceCarrierHz / sampleRate);
  const double sourceStep = 6.28318530717958647692 * ifStrip.sourceCarrierHz / sampleRate;
  ifStrip.sourceDownmixStepCos = static_cast<float>(std::cos(sourceStep));
  ifStrip.sourceDownmixStepSin = static_cast<float>(std::sin(sourceStep));
  ifStrip.ifEnvelopeStepRadians =
      kRadioTwoPi * (computeIfTuneOffsetHz(ifStrip) / sampleRate);
  ifStrip.ifEnvelopeStepCos = std::cos(ifStrip.ifEnvelopeStepRadians);
  ifStrip.ifEnvelopeStepSin = std::sin(ifStrip.ifEnvelopeStepRadians);

  // Bandwidth is a ganged selectivity setting relative to the default 5.5 kHz
  // control position, not a universal measured -3 dB bandwidth. The two receiver
  // presets retain their own coil losses, coupling and number of transformers.
  const float width = safeAudioBw / 5500.0f;
  if (ifStrip.transformerCount < 1 || ifStrip.transformerCount > 3)
    throw std::invalid_argument("IF transformer count must be between 1 and 3");
  for (int can = 0; can < ifStrip.transformerCount; ++can) {
    const float k = can + 1 == ifStrip.transformerCount ? ifStrip.outputCouplingCoeff
                                                        : ifStrip.interstageCouplingCoeff;
    ifStrip.transformers[can].configure(
        4 * sampleRate, ifStrip.ifCenterHz,
        ifStrip.primaryInductanceHenries * radio.identity.ifPrimaryDrift,
        ifStrip.secondaryInductanceHenries * radio.identity.ifSecondaryDrift,
        ifStrip.primarySeriesResistanceOhms * width,
        ifStrip.secondarySeriesResistanceOhms * width,
        ifStrip.secondaryLoadResistanceOhms / width,
        k * width * radio.identity.ifCouplingDrift,
        ifStrip.referenceTransimpedanceOhms / width);
  }
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
  radio.ifStrip.noiseSampleScale = std::sqrt(radio.sampleRate / 48000.0f);
  configureIfStrip(radio);
  // Representation conversion is fixed for this sample rate. Keep the FIR
  // design (including Bessel evaluations) out of ongoing AFC retuning.
  const float sourceBw = radio.tuning.sourceBandwidthHz;
  const float imageHz = std::min(2 * radio.ifStrip.sourceCarrierHz,
                                 radio.sampleRate - 2 * radio.ifStrip.sourceCarrierHz);
  radio.ifStrip.sourceEnvelope.configure(sourceBw / radio.sampleRate,
                                         (imageHz - sourceBw) / radio.sampleRate);
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
                                              mixerConversionGain, ifGain,
                                              ctx.derived.ifInputNoiseAmp);
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
