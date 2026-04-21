#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "playback_main_clock.h"
#include "queues.h"

namespace playback_sync {

struct PrimingAnchor {
  bool valid = false;
  int64_t ptsUs = 0;
};

struct LoopState {
  int64_t frameTimerUs = 0;
  int64_t lastPtsUs = -1;
  int64_t lastDelayUsValue = 33333;
  int64_t lastPresentedTimeUs = 0;
  int lastSerial = 0;
  bool firstPresentedForSerial = false;
  std::vector<int64_t> recentDurations;
};

struct PreparedFrame {
  QueuedFrame frame;
  bool discontinuity = false;
  int64_t delayUs = 0;
  int64_t actualDurationUs = 0;
};

struct FramePlan {
  int64_t delayUs = 0;
  int64_t actualDurationUs = 0;
  int64_t diffUs = 0;
  int64_t targetUs = 0;
  bool syncLost = false;
  bool waitForAudioClock = false;
  bool waitForAudioRecovery = false;
  bool dropForBehind = false;
};

void initializeLoopState(LoopState& state, int initialSerial,
                         int64_t fallbackFrameDurationUs, int64_t nowUs);

void notePlaybackStateChange(LoopState& state, int64_t nowUs);

void syncLoopSerial(LoopState& state, int serial,
                    int64_t fallbackFrameDurationUs, int64_t nowUs);

bool shouldBackoffForEmptyQueue(const LoopState& state, PlayerState playbackState,
                                bool seekPending, int64_t durationUs,
                                int64_t currentPresentedPtsUs, int64_t nowUs);

bool isFrameBackwards(const LoopState& state, const QueuedFrame& frame);

PreparedFrame prepareFrame(LoopState& state, const QueuedFrame& front,
                           const QueuedFrame* next);

FramePlan planFrame(LoopState& state, PlayerState playbackState,
                    const PreparedFrame& prepared,
                    const playback_main_clock::Snapshot& master, int64_t nowUs);

bool shouldDropLateFrame(int64_t nowUs, int64_t targetUs, size_t queueDepth);

void noteLateDrop(LoopState& state, const PreparedFrame& prepared,
                  int64_t targetUs, int64_t nowUs);

void notePresentedFrame(LoopState& state, const PreparedFrame& prepared,
                        int64_t delayUs, int64_t targetUs, int64_t nowUs);

PrimingAnchor choosePrimingAnchor(int64_t audioOldestPtsUs, int64_t videoPtsUs);

}  // namespace playback_sync
