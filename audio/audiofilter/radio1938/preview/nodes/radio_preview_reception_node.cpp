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
  assert(config.seed != 0u);

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
  reception.desiredCarrierI = static_cast<float>(
      config.groundwaveGain + skyGain * runtime.skyOscCos);
  reception.desiredCarrierQ =
      static_cast<float>(skyGain * runtime.skyOscSin);

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

void RadioPreviewReceptionNode::init(RadioPreviewPipeline& preview,
                                     RadioPreviewInitContext&) {
  assert(preview.radio);
  assert(preview.ingress);
  if (!preview.ingress->reception.enabled) return;
  configureReceptionRuntime(preview);
}

void RadioPreviewReceptionNode::reset(RadioPreviewPipeline& preview) {
  assert(preview.ingress);
  auto& runtime = preview.receptionRuntime;
  const auto& config = preview.ingress->reception;
  if (!config.enabled) {
    runtime = {};
    return;
  }
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
  if (!config.enabled) {
    reception = {};
    return y;
  }

  advanceSkywave(preview.receptionRuntime, config, reception);
  reception.additiveRf =
      config.intermittentCarrierEnabled
          ? advanceIntermittentCarrier(preview.receptionRuntime, config)
          : 0.0f;
  return y;
}
