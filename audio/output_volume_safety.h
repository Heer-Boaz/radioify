#ifndef RADIOIFY_AUDIO_OUTPUT_VOLUME_SAFETY_H
#define RADIOIFY_AUDIO_OUTPUT_VOLUME_SAFETY_H

#include <cstdint>

struct OutputVolumeSafetyState {
  float gain = 1.0f;
  uint32_t rampFramesRemaining = 0;
  uint32_t rampFramesTotal = 0;
};

uint32_t outputVolumeSafetyDefaultRampFrames(uint32_t sampleRate);

void primeOutputVolumeSafetyRamp(OutputVolumeSafetyState& state,
                                 uint32_t frames);

void fadeOutputTailToSilence(float* samples,
                             uint32_t audioFrames,
                             uint32_t channels,
                             uint32_t rampFrames);

bool applyOutputVolumeSafety(float* samples,
                             uint32_t frames,
                             uint32_t channels,
                             float volume,
                             uint32_t sampleRate,
                             OutputVolumeSafetyState& state);

#endif  // RADIOIFY_AUDIO_OUTPUT_VOLUME_SAFETY_H
