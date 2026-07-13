#include "../radio_preview_pipeline.h"

#include "../../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

constexpr float kFullModulationSineRms = 0.70710678118654752440f;
constexpr float kUniformNoiseRms = 0.57735026918962576451f;
constexpr uint32_t kNoiseResponseBins = 4096u;
constexpr float kUnsigned24BitInv = 1.0f / 16777215.0f;

double biquadMagnitudeSquared(const Biquad& filter, double radians) {
  const double cos1 = std::cos(radians);
  const double sin1 = std::sin(radians);
  const double cos2 = std::cos(2.0 * radians);
  const double sin2 = std::sin(2.0 * radians);
  const double numeratorReal =
      filter.b0 + filter.b1 * cos1 + filter.b2 * cos2;
  const double numeratorImag = -filter.b1 * sin1 - filter.b2 * sin2;
  const double denominatorReal =
      1.0 + filter.a1 * cos1 + filter.a2 * cos2;
  const double denominatorImag = -filter.a1 * sin1 - filter.a2 * sin2;
  const double numeratorPower = numeratorReal * numeratorReal +
                                numeratorImag * numeratorImag;
  const double denominatorPower = denominatorReal * denominatorReal +
                                  denominatorImag * denominatorImag;
  return numeratorPower / denominatorPower;
}

float programFilterNoiseGain(float sampleRate, float audioBandwidthHz) {
  Biquad hp;
  Biquad lp;
  hp.setHighpass(sampleRate, 45.0f, kRadioBiquadQ);
  lp.setLowpass(sampleRate, audioBandwidthHz, kRadioBiquadQ);

  double powerSum = 0.0;
  for (uint32_t bin = 0; bin < kNoiseResponseBins; ++bin) {
    const double normalizedFrequency =
        (static_cast<double>(bin) + 0.5) /
        static_cast<double>(kNoiseResponseBins);
    const double radians = 3.14159265358979323846 * normalizedFrequency;
    powerSum += biquadMagnitudeSquared(hp, radians) *
                biquadMagnitudeSquared(lp, radians);
  }
  return static_cast<float>(
      std::sqrt(powerSum / static_cast<double>(kNoiseResponseBins)));
}

float envelopeCoefficient(float sampleRate, float timeMs) {
  return std::exp(-1.0f / (sampleRate * timeMs * 0.001f));
}

float nextSignedUniform(uint32_t& state) {
  state ^= state << 13u;
  state ^= state >> 17u;
  state ^= state << 5u;
  const float unit = static_cast<float>(state >> 8u) * kUnsigned24BitInv;
  return 2.0f * unit - 1.0f;
}

float applyCompression(RadioPreviewPipeline& preview,
                       const RadioBroadcastSourceConfig& config,
                       float sample) {
  const float magnitude = std::fabs(sample);
  if (magnitude >= preview.broadcastEnvelope) {
    preview.broadcastEnvelope = magnitude;
  } else {
    preview.broadcastEnvelope = std::max(
        magnitude,
        preview.broadcastReleaseCoefficient * preview.broadcastEnvelope);
  }

  float targetGain = 1.0f;
  if (preview.broadcastEnvelope > preview.broadcastCompressionThreshold) {
    const float overThreshold =
        preview.broadcastEnvelope / preview.broadcastCompressionThreshold;
    const float gainExponent = 1.0f / config.compressionRatio - 1.0f;
    targetGain = std::pow(overThreshold, gainExponent);
  }
  if (targetGain < preview.broadcastGain) {
    preview.broadcastGain =
        preview.broadcastAttackCoefficient * preview.broadcastGain +
        (1.0f - preview.broadcastAttackCoefficient) * targetGain;
  } else {
    preview.broadcastGain = targetGain;
  }
  return sample * preview.broadcastGain;
}

}  // namespace

void RadioPreviewBroadcastSourceNode::init(RadioPreviewPipeline& preview,
                                           RadioPreviewInitContext&) {
  assert(preview.ingress);
  const auto& config = preview.ingress->broadcastSource;
  const float sampleRate = requirePositiveFinite(preview.sampleRate);
  if (config.compressionEnabled) {
    requirePositiveFinite(config.compressionThresholdModulation);
    requirePositiveFinite(preview.ingress->modulationIndex);
    assert(std::isfinite(config.compressionRatio) &&
           config.compressionRatio >= 1.0f);
    requirePositiveFinite(config.compressionAttackMs);
    requirePositiveFinite(config.compressionReleaseMs);
  }
  if (config.nonlinearityEnabled) {
    requireNonNegativeFinite(config.cubicNonlinearity);
  }
  if (config.noiseEnabled) {
    requireNonNegativeFinite(config.noiseBelowFullModulationDb);
    assert(config.noiseSeed != 0u);
  }

  preview.broadcastCompressionThreshold =
      config.compressionThresholdModulation /
      preview.ingress->modulationIndex;

  preview.broadcastAttackCoefficient =
      envelopeCoefficient(sampleRate, config.compressionAttackMs);
  preview.broadcastReleaseCoefficient =
      envelopeCoefficient(sampleRate, config.compressionReleaseMs);

  const float noiseBandGain =
      programFilterNoiseGain(sampleRate, preview.audioBandwidthHz);
  assert(std::isfinite(noiseBandGain) && noiseBandGain > 0.0f);
  const float targetNoiseRms =
      kFullModulationSineRms *
      db2lin(-config.noiseBelowFullModulationDb);
  preview.broadcastNoiseAmplitude =
      targetNoiseRms / (kUniformNoiseRms * noiseBandGain);
}

void RadioPreviewBroadcastSourceNode::reset(RadioPreviewPipeline& preview) {
  assert(preview.ingress);
  preview.broadcastEnvelope = 0.0f;
  preview.broadcastGain = 1.0f;
  preview.broadcastNoiseState = preview.ingress->broadcastSource.noiseSeed;
}

float RadioPreviewBroadcastSourceNode::run(RadioPreviewPipeline& preview,
                                           float y,
                                           RadioPreviewSampleContext&) {
  assert(preview.ingress);
  const auto& config = preview.ingress->broadcastSource;
  if (!config.enabled) return y;

  if (config.compressionEnabled) {
    y = applyCompression(preview, config, y);
  }
  if (config.nonlinearityEnabled) {
    y -= config.cubicNonlinearity * y * y * y;
  }
  if (config.noiseEnabled) {
    y += preview.broadcastNoiseAmplitude *
         nextSignedUniform(preview.broadcastNoiseState);
  }
  return y;
}
