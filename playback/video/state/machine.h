#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "playback/video/frame_step.h"
#include "playback/video/state/types.h"

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
  bool videoReplayPending = false;
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
  void resetFrameSteps();
  void resetForSerial(int serial);
  StateChange requestFrameStep(
      playback_video_frame_step::Direction direction, int serial);
  bool peekFrameStep(playback_video_frame_step::Request* request,
                     int serial, bool frameStepSeekPending);
  bool discardFrameStep(const playback_video_frame_step::Request& request);
  bool publishFrameStepPresentation(
      const playback_video_frame_step::Request& request);
  bool consumeFrameStepPresentation(int serial, uint64_t generation);
  bool publishFrameStepSeek(const playback_video_frame_step::Request& request);
  bool consumeFrameStepSeek(int serial, uint64_t generation);
  bool publishFrameStepSeekPresentation(int serial, uint64_t generation);
  bool resumePlaybackFrameSteps();

 private:
  struct FrameStepToken {
    int serial = 0;
    uint64_t generation = 0;

    bool active() const { return generation != 0; }
    bool matches(int tokenSerial, uint64_t tokenGeneration) const {
      return serial == tokenSerial && generation == tokenGeneration;
    }
    void publish(int tokenSerial, uint64_t tokenGeneration) {
      serial = tokenSerial;
      generation = tokenGeneration;
    }
    void clear() {
      serial = 0;
      generation = 0;
    }
  };

  StateChange set(PlayerState next);
  void clearFrameStepsLocked();
  bool requestMatchesFrontLocked(
      const playback_video_frame_step::Request& request) const;

  std::atomic<int> state_{static_cast<int>(PlayerState::Idle)};
  mutable std::mutex frameStepMutex_;
  std::deque<playback_video_frame_step::Request> frameStepRequests_;
  uint64_t nextFrameStepGeneration_ = 1;
  FrameStepToken pendingPresentedFrameStep_;
  FrameStepToken pendingSeekFrameStep_;
  int frameStepSerial_ = 0;
};

size_t requiredAudioPrefillFrames(uint32_t sampleRate);

bool isPrefillReady(const PipelineSnapshot& snapshot, size_t videoPrefillFrames);

StateProjection project(PlayerState state);

StateEffects stateEffects(PlayerState state);

bool isActivePlaybackState(PlayerState state);

bool isBufferingForUi(PlayerState state);

bool mayPresentVideo(PlayerState state);

}  // namespace playback_video_state_machine
