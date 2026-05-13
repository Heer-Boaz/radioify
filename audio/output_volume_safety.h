#ifndef RADIOIFY_AUDIO_OUTPUT_VOLUME_SAFETY_H
#define RADIOIFY_AUDIO_OUTPUT_VOLUME_SAFETY_H

#include <cstdint>

struct OutputVolumeSafetyState {
  float gain = 1.0f;
};

bool applyOutputVolumeSafety(float* samples,
                             uint32_t frames,
                             uint32_t channels,
                             float volume,
                             uint32_t sampleRate,
                             OutputVolumeSafetyState& state);

#endif  // RADIOIFY_AUDIO_OUTPUT_VOLUME_SAFETY_H
