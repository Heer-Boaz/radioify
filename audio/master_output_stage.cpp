#include "master_output_stage.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>

float processMasterOutputStage(
    float* samples,
    uint32_t frames,
    uint32_t channels,
    float volume) {
  assert(samples != nullptr);
  assert(frames > 0);
  assert(channels > 0);
  assert(std::isfinite(volume));
  assert(volume >= 0.0f && volume <= 4.0f);

  float peakBeforeClipping = 0.0f;
  const size_t sampleCount = static_cast<size_t>(frames) * channels;
  for (size_t index = 0; index < sampleCount; ++index) {
    const float scaled = samples[index] * volume;
    if (!std::isfinite(scaled)) {
      samples[index] = 0.0f;
      continue;
    }

    peakBeforeClipping = std::max(peakBeforeClipping, std::fabs(scaled));
    samples[index] = std::clamp(scaled, -1.0f, 1.0f);
  }

  return peakBeforeClipping;
}
