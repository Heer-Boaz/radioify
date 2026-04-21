#pragma once

#include <cstddef>
#include <cstdint>

#include "player.h"

namespace playback_state_machine {

enum class SessionState {
  Stopped,
  Opening,
  Started,
  Stopping,
  Ended,
  Error,
};

enum class TransportState {
  Stopped,
  Playing,
  Paused,
  Seeking,
};

enum class BufferingState {
  Empty,
  Prefill,
  Priming,
  Ready,
  Draining,
};

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

struct StateEffects {
  bool holdAudioOutput = false;
  bool pauseMainClock = true;
  bool mayPresentVideo = false;
  bool uiBuffering = false;
};

struct StateProjection {
  PlayerState playerState = PlayerState::Idle;
  SessionState session = SessionState::Stopped;
  TransportState transport = TransportState::Stopped;
  BufferingState buffering = BufferingState::Empty;
  StateEffects effects;
};

struct Transition {
  PlayerState nextState = PlayerState::Idle;
  StateProjection projection;
  bool clearSeekFailure = false;
};

size_t requiredAudioPrefillFrames(uint32_t sampleRate);

bool isPrefillReady(const Snapshot& snapshot, size_t videoPrefillFrames);

StateProjection project(PlayerState state);

StateEffects stateEffects(PlayerState state);

bool isActivePlaybackState(PlayerState state);

bool isBufferingForUi(PlayerState state);

bool mayPresentVideo(PlayerState state);

PlayerState resolveSteadyState(Snapshot snapshot, size_t videoPrefillFrames);

Transition resolveTransition(Snapshot snapshot, size_t videoPrefillFrames);

}  // namespace playback_state_machine
