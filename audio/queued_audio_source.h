#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <vector>

#include "playback_backend.h"

struct AudioState;

struct AudioSampleRing {
  std::vector<float> buf;
  uint32_t channels = 0;
  uint64_t capFrames = 0;
  std::atomic<uint64_t> rpos{0};
  std::atomic<uint64_t> wpos{0};

  void init(uint64_t capacityFrames, uint32_t ch);
  void reset();
  uint64_t bufferedFrames() const;
  uint64_t writableFrames() const;
  uint64_t writeSome(const float* in, uint64_t frames);
  uint64_t readSome(float* out, uint64_t frames);
};

struct AudioMetadata {
  uint64_t wpos;
  int64_t ptsUs;
  int serial;
};

bool queuedAudioSourceUsesDecodeThread(const AudioBackendHandlers* backend);
void queuedAudioSourceClearMetadata(AudioState* state);
void queuedAudioSourceReset(AudioState* state, uint64_t framePos, int serial);
void queuedAudioSourceStopWorker(AudioState* state);
bool queuedAudioSourceStartWorker(const AudioBackendHandlers* backend,
                                  uint64_t startFrame);
bool queuedAudioSourceWaitPrimed(AudioState* state, uint32_t primeFrames);
bool queuedAudioSourceCommitPendingSerialFlush(AudioState* state);
int64_t queuedAudioSourceClockStarvationGraceUs(const AudioState* state,
                                                uint32_t frameCount);
bool queuedAudioSourceWrite(AudioState* state,
                            float* interleaved,
                            uint64_t frames,
                            int64_t ptsUs,
                            int serial,
                            bool allowBlock,
                            uint64_t* writtenFrames);
