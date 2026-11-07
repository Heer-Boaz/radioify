#include "../radio_preview_pipeline.h"

#include "../../../../radio.h"
#include "../../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

constexpr double kUniformVariance = 1.0 / 3.0;
constexpr float kUnsigned24BitInv = 1.0f / 16777215.0f;

uint32_t nextRandom(uint32_t& state) {
  state ^= state << 13u;
  state ^= state >> 17u;
  state ^= state << 5u;
  return state;
}

float nextUnit(uint32_t& state) {
  return static_cast<float>(nextRandom(state) >> 8u) * kUnsigned24BitInv;
}

float nextSignedUnit(uint32_t& state) {
  return 2.0f * nextUnit(state) - 1.0f;
}

float randomRange(uint32_t& state, float minimum, float maximum) {
  return minimum + (maximum - minimum) * nextUnit(state);
}

uint64_t secondsToFrames(float seconds, float sampleRate) {
  return static_cast<uint64_t>(
      std::llround(static_cast<double>(seconds) * sampleRate));
}

uint64_t randomFrameRange(uint32_t& state,
                          float minimumSeconds,
                          float maximumSeconds,
                          float sampleRate) {
  return secondsToFrames(
      randomRange(state, minimumSeconds, maximumSeconds), sampleRate);
}

double onePoleCoefficient(float sampleRate, float timeSeconds) {
  return std::exp(-1.0 /
                  (static_cast<double>(sampleRate) * timeSeconds));
}

double unitVarianceExcitation(double pole) {
  return std::sqrt((1.0 - pole * pole) / kUniformVariance);
}

void validateReceptionConfig(const RadioReceptionConfig& config,
                             float sampleRate,
                             float sourceCarrierHz) {
  requirePositiveFinite(sampleRate);
  requirePositiveFinite(sourceCarrierHz);
  requireNonNegativeFinite(config.groundwaveGain);
  requireNonNegativeFinite(config.skywaveGain);
  requireNonNegativeFinite(config.skywaveAmplitudeVariationRms);
  requirePositiveFinite(config.skywaveAmplitudeVariationSeconds);
  requireNonNegativeFinite(config.skywaveDopplerMinHz);
  assert(std::isfinite(config.skywaveDopplerMaxHz) &&
         config.skywaveDopplerMaxHz >= config.skywaveDopplerMinHz);
  requireNonNegativeFinite(config.skywaveDopplerWanderRmsHz);
  requirePositiveFinite(config.skywaveDopplerWanderSeconds);
  requireNonNegativeFinite(config.skywaveDelayMinMs);
  assert(std::isfinite(config.skywaveDelayMaxMs) &&
         config.skywaveDelayMaxMs >= config.skywaveDelayMinMs);
  assert(config.skywaveDelayMaxMs * 0.001f * sampleRate <=
         static_cast<float>(Radio1938::IqInputState::kEnvelopeHistory - 4));
  assert(config.seed != 0u);
  if (config.externalNoiseEnabled) {
    requirePositiveFinite(config.groundwaveFieldVoltsPerMeter);
    requirePositiveFinite(config.groundwaveGain);
    assert(config.externalNoiseSeed != 0u);
  }

  if (!config.intermittentCarrierEnabled) return;
  requireNonNegativeFinite(config.intermittentWaitMinSeconds);
  assert(std::isfinite(config.intermittentWaitMaxSeconds) &&
         config.intermittentWaitMaxSeconds >=
             config.intermittentWaitMinSeconds);
  requirePositiveFinite(config.intermittentDurationMinSeconds);
  assert(std::isfinite(config.intermittentDurationMaxSeconds) &&
         config.intermittentDurationMaxSeconds >=
             config.intermittentDurationMinSeconds);
  assert(std::isfinite(config.intermittentLevelMinDb) &&
         std::isfinite(config.intermittentLevelMaxDb) &&
         config.intermittentLevelMaxDb >= config.intermittentLevelMinDb &&
         config.intermittentLevelMaxDb <= 0.0f);
  requireNonNegativeFinite(config.intermittentOffsetMinHz);
  assert(std::isfinite(config.intermittentOffsetMaxHz) &&
         config.intermittentOffsetMaxHz >= config.intermittentOffsetMinHz);
  requirePositiveFinite(config.intermittentAttackMs);
  requirePositiveFinite(config.intermittentReleaseMs);
  assert(sourceCarrierHz > config.intermittentOffsetMaxHz);
  assert(sourceCarrierHz + config.intermittentOffsetMaxHz <
         0.49f * sampleRate);
}

void configureReceptionRuntime(RadioPreviewPipeline& preview) {
  auto& runtime = preview.receptionRuntime;
  const auto& config = preview.ingress->reception;
  runtime.sampleRate = preview.sampleRate;
  runtime.sourceCarrierHz = preview.radio->resolvedInputCarrierHz();
  runtime.carrierPeak = std::sqrt(2.0f) *
                        preview.ingress->receivedCarrierRmsVolts;
  validateReceptionConfig(config, runtime.sampleRate,
                          runtime.sourceCarrierHz);
  runtime.skyAmplitudePole = onePoleCoefficient(
      runtime.sampleRate, config.skywaveAmplitudeVariationSeconds);
  runtime.skyAmplitudeExcitation =
      unitVarianceExcitation(runtime.skyAmplitudePole);
  runtime.skyDopplerPole = onePoleCoefficient(
      runtime.sampleRate, config.skywaveDopplerWanderSeconds);
  runtime.skyDopplerExcitation =
      unitVarianceExcitation(runtime.skyDopplerPole);
  runtime.intermittentAttackCoefficient = std::exp(
      -1.0f /
      (runtime.sampleRate * config.intermittentAttackMs * 0.001f));
  runtime.intermittentReleaseCoefficient = std::exp(
      -1.0f /
      (runtime.sampleRate * config.intermittentReleaseMs * 0.001f));
  runtime.externalNoiseSigma = 0.0f;
  if (config.externalNoiseEnabled) {
    // White RF noise of one-sided density N0 = 2 sigma^2 / fs against a
    // carrier of power Vrms^2 at unit path gain. The receiver's own
    // selectivity sets the bandwidth that reaches the detector.
    const double cn0DbHz = radioReceptionCarrierToNoiseDensityDbHz(
        config, preview.radio->tuning.stationFrequencyHz);
    runtime.externalNoiseSigma = static_cast<float>(
        preview.ingress->receivedCarrierRmsVolts *
        std::sqrt(0.5 * runtime.sampleRate) * std::pow(10.0, -cn0DbHz / 20.0));
  }
}

float nextGaussian(RadioPreviewPipeline::RadioReceptionRuntime& runtime) {
  if (runtime.externalNoiseSpareValid) {
    runtime.externalNoiseSpareValid = false;
    return runtime.externalNoiseSpare;
  }
  // Box-Muller; the 24-bit uniform is shifted off zero for the logarithm.
  const double u1 = (static_cast<double>(nextRandom(runtime.externalNoiseState) >> 8u) + 0.5) /
                    16777216.0;
  const double u2 = static_cast<double>(nextUnit(runtime.externalNoiseState));
  const double radius = std::sqrt(-2.0 * std::log(u1));
  const double angle = 6.28318530717958647692 * u2;
  runtime.externalNoiseSpare = static_cast<float>(radius * std::sin(angle));
  runtime.externalNoiseSpareValid = true;
  return static_cast<float>(radius * std::cos(angle));
}

void scheduleIntermittentWait(
    RadioPreviewPipeline::RadioReceptionRuntime& runtime,
    const RadioReceptionConfig& config) {
  runtime.intermittentWaitFrames = randomFrameRange(
      runtime.randomState, config.intermittentWaitMinSeconds,
      config.intermittentWaitMaxSeconds, runtime.sampleRate);
  runtime.intermittentWaitFrames =
      std::max<uint64_t>(uint64_t{1}, runtime.intermittentWaitFrames);
}

void startIntermittentCarrier(
    RadioPreviewPipeline::RadioReceptionRuntime& runtime,
    const RadioReceptionConfig& config) {
  runtime.intermittentActiveFrames = std::max<uint64_t>(
      uint64_t{1}, randomFrameRange(runtime.randomState,
                                    config.intermittentDurationMinSeconds,
                                    config.intermittentDurationMaxSeconds,
                                    runtime.sampleRate));
  runtime.intermittentTarget = db2lin(randomRange(
      runtime.randomState, config.intermittentLevelMinDb,
      config.intermittentLevelMaxDb));
  const float offsetMagnitude = randomRange(
      runtime.randomState, config.intermittentOffsetMinHz,
      config.intermittentOffsetMaxHz);
  const float offsetSign =
      nextSignedUnit(runtime.randomState) < 0.0f ? -1.0f : 1.0f;
  runtime.intermittentOffsetHz = offsetSign * offsetMagnitude;
  const float carrierHz =
      runtime.sourceCarrierHz + runtime.intermittentOffsetHz;
  const float step = kRadioTwoPi * carrierHz / runtime.sampleRate;
  runtime.intermittentStepCos = std::cos(step);
  runtime.intermittentStepSin = std::sin(step);
  const float phase = kRadioTwoPi * nextUnit(runtime.randomState);
  runtime.intermittentOscCos = std::cos(phase);
  runtime.intermittentOscSin = std::sin(phase);
}

void advanceSkywave(RadioPreviewPipeline::RadioReceptionRuntime& runtime,
                    const RadioReceptionConfig& config,
                    RadioAmReceptionSample& reception) {
  runtime.skyAmplitudeState =
      runtime.skyAmplitudePole * runtime.skyAmplitudeState +
      runtime.skyAmplitudeExcitation *
          static_cast<double>(nextSignedUnit(runtime.randomState));
  runtime.skyDopplerState =
      runtime.skyDopplerPole * runtime.skyDopplerState +
      runtime.skyDopplerExcitation *
          static_cast<double>(nextSignedUnit(runtime.randomState));
  runtime.skyAmplitudeState =
      std::clamp(runtime.skyAmplitudeState, -3.0, 3.0);
  runtime.skyDopplerState =
      std::clamp(runtime.skyDopplerState, -3.0, 3.0);

  const double amplitudeScale = std::clamp(
      1.0 + static_cast<double>(config.skywaveAmplitudeVariationRms) *
                runtime.skyAmplitudeState,
      0.35, 1.65);
  const double skyGain =
      static_cast<double>(config.skywaveGain) * amplitudeScale;
  reception.desiredCarrierI = config.groundwaveGain;
  reception.desiredCarrierQ = 0.0f;
  reception.skywaveI = static_cast<float>(skyGain * runtime.skyOscCos);
  reception.skywaveQ = static_cast<float>(skyGain * runtime.skyOscSin);
  reception.skywaveDelaySamples = runtime.skyDelaySamples;

  const double dopplerHz =
      runtime.skyDopplerBaseHz +
      config.skywaveDopplerWanderRmsHz * runtime.skyDopplerState;
  const double phaseStep =
      static_cast<double>(kRadioTwoPi) * dopplerHz / runtime.sampleRate;
  const double stepCos = 1.0 - 0.5 * phaseStep * phaseStep;
  const double nextCos = runtime.skyOscCos * stepCos -
                         runtime.skyOscSin * phaseStep;
  const double nextSin = runtime.skyOscSin * stepCos +
                         runtime.skyOscCos * phaseStep;
  const double magnitudeSq = nextCos * nextCos + nextSin * nextSin;
  const double renorm = 1.5 - 0.5 * magnitudeSq;
  runtime.skyOscCos = nextCos * renorm;
  runtime.skyOscSin = nextSin * renorm;
}

float advanceIntermittentCarrier(
    RadioPreviewPipeline::RadioReceptionRuntime& runtime,
    const RadioReceptionConfig& config) {
  if (runtime.intermittentActiveFrames > 0u) {
    --runtime.intermittentActiveFrames;
    if (runtime.intermittentActiveFrames == 0u) {
      runtime.intermittentTarget = 0.0f;
      scheduleIntermittentWait(runtime, config);
    }
  } else if (runtime.intermittentWaitFrames > 0u) {
    --runtime.intermittentWaitFrames;
    if (runtime.intermittentWaitFrames == 0u) {
      startIntermittentCarrier(runtime, config);
    }
  }

  const float coefficient =
      runtime.intermittentTarget > runtime.intermittentEnvelope
          ? runtime.intermittentAttackCoefficient
          : runtime.intermittentReleaseCoefficient;
  runtime.intermittentEnvelope =
      coefficient * runtime.intermittentEnvelope +
      (1.0f - coefficient) * runtime.intermittentTarget;
  if (runtime.intermittentEnvelope <= 1e-9f) return 0.0f;

  const float rf = runtime.carrierPeak * runtime.intermittentEnvelope *
                   runtime.intermittentOscCos;
  advanceNormalizedOscillator(
      runtime.intermittentStepCos, runtime.intermittentStepSin,
      runtime.intermittentOscCos, runtime.intermittentOscSin);
  return rf;
}

}  // namespace

std::array<float, 2> radioPreviewInitialCarrierGain(const RadioPreviewPipeline& preview) {
  const auto& config = preview.ingress->reception;
  if (!config.enabled) return {1.0f, 0.0f};
  // Groundwave plus the skywave phasor as advanceSkywave first emits it.
  const auto& runtime = preview.receptionRuntime;
  return {config.groundwaveGain + config.skywaveGain * static_cast<float>(runtime.skyOscCos),
          config.skywaveGain * static_cast<float>(runtime.skyOscSin)};
}

void RadioPreviewReceptionNode::init(RadioPreviewPipeline& preview,
                                     RadioPreviewInitContext&) {
  assert(preview.radio);
  assert(preview.ingress);
  const auto& config = preview.ingress->reception;
  if (!config.enabled && !config.externalNoiseEnabled) return;
  configureReceptionRuntime(preview);
}

void RadioPreviewReceptionNode::reset(RadioPreviewPipeline& preview) {
  assert(preview.ingress);
  auto& runtime = preview.receptionRuntime;
  const auto& config = preview.ingress->reception;
  if (!config.enabled && !config.externalNoiseEnabled) {
    runtime = {};
    return;
  }
  // The noise has its own stream so the fading realisation of a seed is
  // unchanged by it.
  runtime.externalNoiseState = config.externalNoiseSeed;
  runtime.externalNoiseSpareValid = false;
  if (!config.enabled) return;
  runtime.randomState = config.seed;
  runtime.skyAmplitudeState = 0.0;
  runtime.skyDopplerState = 0.0;
  const float skyPhase = kRadioTwoPi * nextUnit(runtime.randomState);
  runtime.skyOscCos = std::cos(skyPhase);
  runtime.skyOscSin = std::sin(skyPhase);
  const float dopplerMagnitude = randomRange(
      runtime.randomState, config.skywaveDopplerMinHz,
      config.skywaveDopplerMaxHz);
  runtime.skyDopplerBaseHz =
      nextSignedUnit(runtime.randomState) < 0.0f ? -dopplerMagnitude
                                                : dopplerMagnitude;
  // The path geometry gets its own stream so the fading realisation of a
  // seed is unchanged by it.
  uint32_t geometryState = config.seed ^ 0x9E3779B9u;
  if (geometryState == 0u) geometryState = 1u;
  runtime.skyDelaySamples =
      0.001f * runtime.sampleRate *
      randomRange(geometryState, config.skywaveDelayMinMs, config.skywaveDelayMaxMs);
  runtime.intermittentActiveFrames = 0u;
  runtime.intermittentEnvelope = 0.0f;
  runtime.intermittentTarget = 0.0f;
  runtime.intermittentOffsetHz = 0.0f;
  runtime.intermittentOscCos = 1.0f;
  runtime.intermittentOscSin = 0.0f;
  if (config.intermittentCarrierEnabled) {
    scheduleIntermittentWait(runtime, config);
  } else {
    runtime.intermittentWaitFrames = 0u;
  }
}

float RadioPreviewReceptionNode::run(RadioPreviewPipeline& preview,
                                     float y,
                                     RadioPreviewSampleContext& ctx) {
  assert(preview.ingress);
  assert(ctx.reception);
  auto& reception = *ctx.reception;
  const auto& config = preview.ingress->reception;
  auto& runtime = preview.receptionRuntime;
  if (config.enabled) {
    advanceSkywave(runtime, config, reception);
    reception.additiveRf =
        config.intermittentCarrierEnabled
            ? advanceIntermittentCarrier(runtime, config)
            : 0.0f;
  } else {
    reception = {};
  }
  if (config.externalNoiseEnabled)
    reception.additiveRf += runtime.externalNoiseSigma * nextGaussian(runtime);
  return y;
}
