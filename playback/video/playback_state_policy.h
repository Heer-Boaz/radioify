#pragma once

#include <cstddef>
#include <cstdint>

#include "player.h"

namespace playback_state_policy {

struct Snapshot {
  PlayerState currentState = PlayerState::Idle;
  bool initDone = false;
  bool initOk = false;
  bool audioPaused = false;
  bool audioStartedOk = false;
  bool audioFinished = false;
  bool audioDecodeEnded = false;
  bool decodeEnded = false;
  bool demuxEnded = false;
  bool videoQueueEmpty = true;
  size_t videoQueueDepth = 0;
  size_t audioBufferedFrames = 0;
  uint32_t audioSampleRate = 0;
  bool audioSyncPointReady = false;
  int currentSerial = 0;
  int lastPresentedSerial = 0;
  int pendingSeekSerial = 0;
  int seekInFlightSerial = 0;
  bool seekFailed = false;
};

size_t requiredAudioPrefillFrames(uint32_t sampleRate);

bool isPrefillReady(const Snapshot& snapshot, size_t videoPrefillFrames);

bool shouldHoldAudioOutput(PlayerState state);

bool shouldPauseMainClock(PlayerState state);

PlayerState resolveSteadyState(Snapshot snapshot, size_t videoPrefillFrames);

}  // namespace playback_state_policy
