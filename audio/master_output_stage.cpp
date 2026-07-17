#include "master_output_stage.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

constexpr float kOutputCeiling = 0.980f;  // -0.18 dBFS device headroom.
constexpr float kLimiterReleaseSeconds = 0.250f;

float sanitizeVolume(float volume) {
  return std::isfinite(volume) ? std::max(volume, 0.0f) : 0.0f;
}

}  // namespace

MasterOutputStageResult processMasterOutputStage(
    float* samples,
    uint32_t frames,
    uint32_t channels,
    float volume,
    uint32_t sampleRate,
    uint64_t resetGeneration,
    MasterOutputStageState& state) {
  MasterOutputStageResult result;
  if (!samples || frames == 0 || channels == 0) return result;

  if (state.appliedResetGeneration != resetGeneration ||
      !std::isfinite(state.limiterGain) || state.limiterGain <= 0.0f ||
      state.limiterGain > 1.0f) {
    state.limiterGain = 1.0f;
    state.appliedResetGeneration = resetGeneration;
  }

  const float outputVolume = sanitizeVolume(volume);
  const float rate = static_cast<float>(std::max(sampleRate, 1u));
  const float releaseCoefficient =
      std::exp(-1.0f / (rate * kLimiterReleaseSeconds));

  for (uint32_t frame = 0; frame < frames; ++frame) {
    const size_t frameOffset = static_cast<size_t>(frame) * channels;
    float framePeak = 0.0f;
    for (uint32_t channel = 0; channel < channels; ++channel) {
      float scaled = samples[frameOffset + channel] * outputVolume;
      if (!std::isfinite(scaled)) scaled = 0.0f;
      samples[frameOffset + channel] = scaled;
      framePeak = std::max(framePeak, std::fabs(scaled));
    }

    const float requiredGain =
        framePeak > kOutputCeiling
            ? kOutputCeiling / std::max(framePeak, 1e-9f)
            : 1.0f;
    if (requiredGain < state.limiterGain) {
      state.limiterGain = requiredGain;
    } else {
      state.limiterGain =
          releaseCoefficient * state.limiterGain +
          (1.0f - releaseCoefficient);
      state.limiterGain = std::min(state.limiterGain, requiredGain);
    }

    for (uint32_t channel = 0; channel < channels; ++channel) {
      const size_t index = frameOffset + channel;
      const float output = std::clamp(samples[index] * state.limiterGain,
                                      -kOutputCeiling, kOutputCeiling);
      samples[index] = output;
      result.outputPeak = std::max(result.outputPeak, std::fabs(output));
    }
  }

  return result;
}
