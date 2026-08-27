#pragma once

#include <cstddef>
#include <cstdint>

#include "stream_reset.h"

struct AudioPlaybackState;

bool audioStartStream(AudioPlaybackState& audio, uint64_t totalFrames);
void audioStopStream(AudioPlaybackState& audio);
size_t audioStreamBufferedFrames(AudioPlaybackState& audio);
size_t audioStreamCapacityFrames(AudioPlaybackState& audio);
int64_t audioStreamOldestPtsUs(AudioPlaybackState& audio);
bool audioStreamWriteSamples(AudioPlaybackState& audio,
                             const float* interleaved, uint64_t frames,
                             int64_t ptsUs, int serial, bool allowBlock,
                             uint64_t* writtenFrames);
void audioStreamPrimeClock(AudioPlaybackState& audio, int serial,
                           int64_t targetPtsUs);
void audioStreamSetEnd(AudioPlaybackState& audio, bool atEnd);
void audioStreamReset(AudioPlaybackState& audio, uint64_t framePos);
void audioStreamFlushSerial(AudioPlaybackState& audio, int serial,
                            int64_t discardUntilUs);
AudioStreamReset audioStreamLastAppliedReset(AudioPlaybackState& audio);
int audioStreamSerial(AudioPlaybackState& audio);
int64_t audioStreamClockUs(AudioPlaybackState& audio, int64_t nowUs);
int64_t audioStreamClockLastUpdatedUs(AudioPlaybackState& audio);
bool audioStreamStarved(AudioPlaybackState& audio);
bool audioStreamClockReady(AudioPlaybackState& audio);
uint64_t audioStreamWaitForUpdate(AudioPlaybackState& audio,
                                  uint64_t lastCounter, int timeoutMs);
uint64_t audioStreamUpdateCounter(AudioPlaybackState& audio);
