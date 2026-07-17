#ifndef RADIOIFY_AUDIO_MASTER_OUTPUT_STAGE_H
#define RADIOIFY_AUDIO_MASTER_OUTPUT_STAGE_H

#include <cstdint>

// Applies user volume, clamps only device overrange, and returns the peak that
// existed before that clamp. Exactly +/-1.0 is valid full-scale PCM.
float processMasterOutputStage(
    float* samples,
    uint32_t frames,
    uint32_t channels,
    float volume);

#endif  // RADIOIFY_AUDIO_MASTER_OUTPUT_STAGE_H
