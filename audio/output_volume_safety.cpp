#include "output_volume_safety.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>

uint32_t outputVolumeSafetyDefaultRampFrames(uint32_t sampleRate) {
  constexpr float kDeclickRampSeconds = 0.010f;
  const float rate = static_cast<float>(std::max(sampleRate, 1u));
  return std::max(1u, static_cast<uint32_t>(std::lround(
                         rate * kDeclickRampSeconds)));
}

void primeOutputVolumeSafetyRamp(OutputVolumeSafetyState& state,
                                 uint32_t frames) {
  if (frames == 0) return;
  state.rampFramesRemaining = frames;
  state.rampFramesTotal = frames;
}

void fadeOutputTailToSilence(float* samples,
                             uint32_t audioFrames,
                             uint32_t channels,
                             uint32_t rampFrames) {
  if (!samples || audioFrames == 0 || channels == 0 || rampFrames == 0) {
    return;
  }

  const uint32_t fadeFrames = std::min(audioFrames, rampFrames);
  const uint32_t firstFadeFrame = audioFrames - fadeFrames;
  for (uint32_t frame = 0; frame < fadeFrames; ++frame) {
    const float gain =
        static_cast<float>(fadeFrames - frame - 1) /
        static_cast<float>(fadeFrames);
    const size_t frameOffset =
        static_cast<size_t>(firstFadeFrame + frame) * channels;
    for (uint32_t ch = 0; ch < channels; ++ch) {
      samples[frameOffset + ch] *= gain;
    }
  }
}

static float consumeRampGain(OutputVolumeSafetyState& state) {
  if (state.rampFramesRemaining == 0 || state.rampFramesTotal == 0) {
    state.rampFramesRemaining = 0;
    state.rampFramesTotal = 0;
    return 1.0f;
  }

  const uint32_t total = std::max(state.rampFramesTotal, 1u);
  const uint32_t remaining = std::min(state.rampFramesRemaining, total);
  const uint32_t frameIndex = total - remaining;
  const float gain =
      static_cast<float>(frameIndex + 1) / static_cast<float>(total);
  state.rampFramesRemaining = remaining - 1;
  if (state.rampFramesRemaining == 0) {
    state.rampFramesTotal = 0;
  }
  return std::clamp(gain, 0.0f, 1.0f);
}

bool applyOutputVolumeSafety(float* samples,
                             uint32_t frames,
                             uint32_t channels,
                             float volume,
                             uint32_t sampleRate,
                             OutputVolumeSafetyState& state) {
  if (!samples || frames == 0 || channels == 0) return false;

  constexpr float kOutputCeiling = 0.980f;
  constexpr float kDeviceClamp = 0.999f;
  constexpr float kReleaseSeconds = 0.250f;

  const size_t count = static_cast<size_t>(frames) * channels;
  const float safeVolume = std::max(volume, 0.0f);
  float rawPeak = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    const float x = samples[i] * safeVolume;
    if (!std::isfinite(x)) {
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
  bool limited = rawPeak > kOutputCeiling || state.gain < 0.999f;
  for (uint32_t frame = 0; frame < frames; ++frame) {
    const float frameGain = outputGain * consumeRampGain(state);
    const size_t frameOffset = static_cast<size_t>(frame) * channels;
    for (uint32_t ch = 0; ch < channels; ++ch) {
      const size_t i = frameOffset + ch;
      float y = samples[i] * frameGain;
      if (!std::isfinite(y)) {
        y = 0.0f;
        limited = true;
      }
      const float clamped = std::clamp(y, -kDeviceClamp, kDeviceClamp);
      if (clamped != y) limited = true;
      samples[i] = clamped;
    }
  }
  return limited;
}
