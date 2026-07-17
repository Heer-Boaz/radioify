#ifndef RADIOIFY_AUDIO_MASTER_OUTPUT_STAGE_H
#define RADIOIFY_AUDIO_MASTER_OUTPUT_STAGE_H

#include <cstdint>

// Callback-owned DSP state. Control threads request a reset by publishing a
// new generation; they never mutate this state directly.
struct MasterOutputStageState {
  float limiterGain = 1.0f;
  uint64_t appliedResetGeneration = 0;
};

struct MasterOutputStageResult {
  float outputPeak = 0.0f;
};

MasterOutputStageResult processMasterOutputStage(
    float* samples,
    uint32_t frames,
    uint32_t channels,
    float volume,
    uint32_t sampleRate,
    uint64_t resetGeneration,
    MasterOutputStageState& state);

#endif  // RADIOIFY_AUDIO_MASTER_OUTPUT_STAGE_H
