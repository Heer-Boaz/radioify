#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "playback/video/player.h"

namespace playback_video_state_machine {

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

struct PipelineSnapshot {
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

struct StateChange {
  PlayerState previous = PlayerState::Idle;
  PlayerState current = PlayerState::Idle;
  StateProjection projection;
  bool changed = false;
};

struct Evaluation {
  StateChange change;
  bool clearSeekFailure = false;
};

class Controller {
 public:
  void reset();
  PlayerState current() const;
  StateProjection projection() const;
  StateChange beginOpening();
  StateChange beginSeeking();
  StateChange finishPriming();
  StateChange beginClosing();
  StateChange finishClosing();
  Evaluation observe(const PipelineSnapshot& snapshot,
                     size_t videoPrefillFrames);

 private:
  StateChange set(PlayerState next);

  std::atomic<int> state_{static_cast<int>(PlayerState::Idle)};
};

size_t requiredAudioPrefillFrames(uint32_t sampleRate);

bool isPrefillReady(const PipelineSnapshot& snapshot, size_t videoPrefillFrames);

StateProjection project(PlayerState state);

StateEffects stateEffects(PlayerState state);

bool isActivePlaybackState(PlayerState state);

bool isBufferingForUi(PlayerState state);

bool mayPresentVideo(PlayerState state);

}  // namespace playback_video_state_machine
