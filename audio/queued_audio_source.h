#pragma once

#include <cstdint>

#include "playback_backend.h"

struct AudioState;

struct AudioStreamResetRequest {
  uint64_t generation = 0;
  int serial = 0;
  int64_t discardUntilUs = 0;
  uint64_t framePosition = 0;
  bool resetPlaybackPosition = false;
};

bool queuedAudioSourceUsesDecoderWorker(const AudioBackendHandlers* backend);
void queuedAudioSourceStartProcessing(AudioState* state,
                                      uint64_t outputCapacityFrames,
                                      uint32_t outputTargetFrames,
                                      uint32_t primeFrames,
                                      uint64_t framePosition,
                                      int serial);
void queuedAudioSourceStopProcessing(AudioState* state);
void queuedAudioSourceStopDecoderWorker(AudioState* state);
bool queuedAudioSourceStartDecoderWorker(const AudioBackendHandlers* backend,
                                         uint64_t startFrame);
bool queuedAudioSourceWaitPrimed(AudioState* state, uint32_t primeFrames);
uint64_t queuedAudioSourceRequestSourceReset(AudioState* state,
                                             uint64_t framePosition);
bool queuedAudioSourceWaitForSourceReset(AudioState* state,
                                         uint64_t generation);
uint64_t queuedAudioSourceRequestStreamReset(AudioState* state,
                                             int serial,
                                             int64_t discardUntilUs,
                                             uint64_t framePosition,
                                             bool resetPlaybackPosition);
bool queuedAudioSourceWaitForStreamReset(AudioState* state,
                                         uint64_t generation);
void queuedAudioSourceCancelStreamResets(AudioState* state);
int64_t queuedAudioSourceClockStarvationGraceUs(const AudioState* state,
                                                uint32_t frameCount);
bool queuedAudioSourceWrite(AudioState* state,
                            const float* interleaved,
                            uint64_t frames,
                            int64_t ptsUs,
                            int serial,
                            bool allowBlock,
                            bool allowCommitInProgressWrite,
                            uint64_t* writtenFrames);
