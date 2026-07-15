#include "output_volume_safety.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>

OutputVolumeSafetyResult applyOutputVolumeSafety(
    float* samples,
    uint32_t frames,
    uint32_t channels,
    float volume,
    uint32_t sampleRate,
    OutputVolumeSafetyState& state) {
  OutputVolumeSafetyResult result;
  if (!samples || frames == 0 || channels == 0) return result;

  constexpr float kOutputCeiling = 0.980f;
  constexpr float kDeviceClamp = 0.999f;
  constexpr float kReleaseSeconds = 0.250f;

  const size_t count = static_cast<size_t>(frames) * channels;
  const float safeVolume = std::max(volume, 0.0f);
  float rawPeak = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    const float x = samples[i] * safeVolume;
    if (!std::isfinite(x)) {
      result.inputOverrange = true;
      rawPeak = std::numeric_limits<float>::infinity();
      break;
    }
    rawPeak = std::max(rawPeak, std::fabs(x));
  }

  float targetGain = 1.0f;
  if (rawPeak > kOutputCeiling) {
    targetGain = kOutputCeiling / std::max(rawPeak, 1e-9f);
  }

  if (!std::isfinite(state.gain) || state.gain <= 0.0f) {
    state.gain = 1.0f;
  }
  if (targetGain < state.gain) {
    state.gain = targetGain;
  } else {
    const float rate = static_cast<float>(std::max(sampleRate, 1u));
    const float releaseCoeff =
        std::exp(-static_cast<float>(frames) / (rate * kReleaseSeconds));
    state.gain = releaseCoeff * state.gain + (1.0f - releaseCoeff);
    state.gain = std::min(state.gain, targetGain);
  }

  const float outputGain = safeVolume * state.gain;
  result.gainReductionActive =
      rawPeak > kOutputCeiling || state.gain < 0.999f;
  result.inputOverrange = result.inputOverrange || rawPeak > 1.0f;
  for (size_t i = 0; i < count; ++i) {
    float y = samples[i] * outputGain;
    if (!std::isfinite(y)) {
      y = 0.0f;
      result.inputOverrange = true;
    }
    const float clamped = std::clamp(y, -kDeviceClamp, kDeviceClamp);
    if (clamped != y) result.inputOverrange = true;
    samples[i] = clamped;
  }
  return result;
}
