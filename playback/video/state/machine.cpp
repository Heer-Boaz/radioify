#include "machine.h"

namespace playback_video_state_machine {
namespace {

struct Transition {
  PlayerState nextState = PlayerState::Idle;
  StateProjection projection;
  bool clearSeekFailure = false;
};

StateChange unchanged(PlayerState state) {
  StateChange change;
  change.previous = state;
  change.current = state;
  change.projection = project(state);
  return change;
}

StateProjection makeProjection(PlayerState playerState, SessionState session,
                               TransportState transport,
                               BufferingState buffering) {
  StateProjection projection;
  projection.playerState = playerState;
  projection.session = session;
  projection.transport = transport;
  projection.buffering = buffering;

  projection.effects.holdAudioOutput =
      session == SessionState::Opening ||
      buffering == BufferingState::Prefill ||
      buffering == BufferingState::Priming ||
      transport == TransportState::Seeking;
  projection.effects.pauseMainClock =
      session != SessionState::Started ||
      buffering == BufferingState::Prefill ||
      transport == TransportState::Paused;
  projection.effects.mayPresentVideo =
      transport == TransportState::Seeking ||
      (transport == TransportState::Playing &&
       buffering != BufferingState::Empty &&
       buffering != BufferingState::Prefill);
  projection.effects.uiBuffering =
      session == SessionState::Opening ||
      buffering == BufferingState::Prefill ||
      buffering == BufferingState::Priming ||
      transport == TransportState::Seeking;

  return projection;
}

}  // namespace

size_t requiredAudioPrefillFrames(uint32_t sampleRate) {
  uint32_t effectiveSampleRate = sampleRate > 0 ? sampleRate : 48000;
  return static_cast<size_t>(effectiveSampleRate / 3);
}

bool isPrefillReady(const PipelineSnapshot& snapshot,
                    size_t videoPrefillFrames) {
  bool videoReady =
      snapshot.videoQueueDepth >= videoPrefillFrames || snapshot.decodeEnded;
  bool audioHasDecoded =
      snapshot.audioBufferedFrames >=
          requiredAudioPrefillFrames(snapshot.audioSampleRate) &&
      snapshot.audioSyncPointReady;
  bool audioReady =
      !snapshot.audioStartedOk || snapshot.audioDecodeEnded || audioHasDecoded;
  return videoReady && audioReady;
}

bool canBeginSeekingFrom(SessionState session) {
  return session == SessionState::Started || session == SessionState::Ended;
}

bool canClaimFrameStep(PlayerState state) {
  TransportState transport = project(state).transport;
  return transport == TransportState::Paused ||
         transport == TransportState::Seeking;
}

StateProjection project(PlayerState state) {
  switch (state) {
    case PlayerState::Idle:
      return makeProjection(state, SessionState::Stopped,
                            TransportState::Stopped, BufferingState::Empty);
    case PlayerState::Opening:
      return makeProjection(state, SessionState::Opening,
                            TransportState::Stopped, BufferingState::Empty);
    case PlayerState::Prefill:
      return makeProjection(state, SessionState::Started,
                            TransportState::Playing, BufferingState::Prefill);
    case PlayerState::Priming:
      return makeProjection(state, SessionState::Started,
                            TransportState::Playing, BufferingState::Priming);
    case PlayerState::Playing:
      return makeProjection(state, SessionState::Started,
                            TransportState::Playing, BufferingState::Ready);
    case PlayerState::Paused:
    case PlayerState::FrameStep:
      return makeProjection(state, SessionState::Started,
                            TransportState::Paused, BufferingState::Ready);
    case PlayerState::Seeking:
      return makeProjection(state, SessionState::Started,
                            TransportState::Seeking, BufferingState::Prefill);
    case PlayerState::Draining:
      return makeProjection(state, SessionState::Started,
                            TransportState::Playing, BufferingState::Draining);
    case PlayerState::Ended:
      return makeProjection(state, SessionState::Ended, TransportState::Stopped,
                            BufferingState::Empty);
    case PlayerState::Error:
      return makeProjection(state, SessionState::Error, TransportState::Stopped,
                            BufferingState::Empty);
    case PlayerState::Closing:
      return makeProjection(state, SessionState::Stopping,
                            TransportState::Stopped, BufferingState::Empty);
  }

  return makeProjection(PlayerState::Idle, SessionState::Stopped,
                        TransportState::Stopped, BufferingState::Empty);
}

void Controller::reset() {
  state_.store(static_cast<int>(PlayerState::Idle), std::memory_order_relaxed);
  resetFrameSteps();
}

PlayerState Controller::current() const {
  return static_cast<PlayerState>(state_.load(std::memory_order_relaxed));
}

StateProjection Controller::projection() const {
  return project(current());
}

StateChange Controller::set(PlayerState next) {
  PlayerState previous = static_cast<PlayerState>(
      state_.exchange(static_cast<int>(next), std::memory_order_relaxed));
  StateChange change;
  change.previous = previous;
  change.current = next;
  change.changed = previous != next;
  change.projection = project(next);
  return change;
}

StateChange Controller::beginOpening() {
  PlayerState state = current();
  if (state != PlayerState::Idle) {
    return unchanged(state);
  }
  return set(PlayerState::Opening);
}

StateChange Controller::beginSeeking() {
  StateProjection currentProjection = projection();
  if (!canBeginSeekingFrom(currentProjection.session)) {
    return unchanged(currentProjection.playerState);
  }
  return set(PlayerState::Seeking);
}

StateChange Controller::finishPriming() {
  PlayerState state = current();
  if (state != PlayerState::Priming) {
    return unchanged(state);
  }
  return set(PlayerState::Playing);
}

StateChange Controller::beginClosing() {
  PlayerState state = current();
  if (state == PlayerState::Idle) {
    return unchanged(state);
  }
  return set(PlayerState::Closing);
}

StateChange Controller::finishClosing() {
  PlayerState state = current();
  if (state != PlayerState::Closing) {
    return unchanged(state);
  }
  return set(PlayerState::Idle);
}

StateEffects stateEffects(PlayerState state) {
  return project(state).effects;
}

bool isActivePlaybackState(PlayerState state) {
  StateProjection projection = project(state);
  return projection.session == SessionState::Started &&
         projection.transport == TransportState::Playing;
}

bool isBufferingForUi(PlayerState state) {
  return project(state).effects.uiBuffering;
}

bool mayPresentVideo(PlayerState state) {
  return project(state).effects.mayPresentVideo;
}

PlayerState resolveSteadyState(PlayerState currentState,
                               PipelineSnapshot snapshot,
                               size_t videoPrefillFrames) {
  while (true) {
    PlayerState nextState = currentState;

    if (currentState == PlayerState::Opening && snapshot.initDone) {
      nextState = snapshot.initOk
                      ? (snapshot.audioPaused ? PlayerState::Paused
                                              : PlayerState::Prefill)
                      : PlayerState::Error;
    } else if (currentState == PlayerState::Seeking &&
               snapshot.seekInFlightSerial == 0) {
      if (snapshot.seekFailed) {
        nextState =
            snapshot.audioPaused ? PlayerState::Paused : PlayerState::Prefill;
      } else if (snapshot.audioPaused &&
                 snapshot.pendingSeekSerial == snapshot.currentSerial &&
                 !(snapshot.decodeEnded && snapshot.videoQueueEmpty)) {
        nextState = PlayerState::Seeking;
      } else {
        nextState =
            snapshot.audioPaused ? PlayerState::Paused : PlayerState::Prefill;
      }
    } else if (isActivePlaybackState(currentState) && snapshot.audioPaused) {
      nextState = PlayerState::Paused;
    } else if ((currentState == PlayerState::Paused ||
                currentState == PlayerState::FrameStep) &&
               !snapshot.audioPaused) {
      nextState = snapshot.lastPresentedSerial == snapshot.currentSerial
                      ? PlayerState::Playing
                      : PlayerState::Prefill;
    } else if (currentState == PlayerState::Prefill &&
               !snapshot.audioPaused &&
               isPrefillReady(snapshot, videoPrefillFrames)) {
      nextState = PlayerState::Priming;
    } else if (isActivePlaybackState(currentState)) {
      bool audioActive = snapshot.audioStartedOk && !snapshot.audioFinished;
      bool ended = snapshot.decodeEnded && snapshot.videoQueueEmpty &&
                   !snapshot.videoReplayPending &&
                   (!audioActive || snapshot.audioFinished);
      if (ended) {
        nextState = PlayerState::Ended;
      } else if (snapshot.demuxEnded &&
                 currentState == PlayerState::Playing &&
                 snapshot.videoQueueEmpty && !snapshot.videoReplayPending) {
        nextState = PlayerState::Draining;
      }
    }

    if (nextState == currentState) {
      return nextState;
    }
    currentState = nextState;
  }
}

Transition resolveTransition(PlayerState currentState,
                             PipelineSnapshot snapshot,
                             size_t videoPrefillFrames) {
  Transition transition;
  transition.nextState = resolveSteadyState(
      currentState, snapshot, videoPrefillFrames);
  transition.projection = project(transition.nextState);
  transition.clearSeekFailure = currentState == PlayerState::Seeking &&
                                transition.nextState != PlayerState::Seeking &&
                                snapshot.seekInFlightSerial == 0;
  return transition;
}

Evaluation Controller::observe(const PipelineSnapshot& snapshot,
                               size_t videoPrefillFrames) {
  Evaluation evaluation;
  PlayerState state = current();
  Transition transition =
      resolveTransition(state, snapshot, videoPrefillFrames);
  evaluation.clearSeekFailure = transition.clearSeekFailure;
  if (transition.nextState != state) {
    evaluation.change = set(transition.nextState);
  } else {
    evaluation.change = unchanged(state);
  }
  return evaluation;
}

void Controller::resetFrameSteps() {
  std::lock_guard<std::mutex> lock(frameStepMutex_);
  frameStepSerial_ = 0;
  clearFrameStepsLocked();
  frameStepResumeReanchorRequired_ = false;
}

void Controller::resetForSerial(int serial) {
  std::lock_guard<std::mutex> lock(frameStepMutex_);
  frameStepSerial_ = serial;
  clearFrameStepsLocked();
  frameStepResumeReanchorRequired_ = false;
}

void Controller::resetForFrameStepSeekSerial(int serial) {
  std::lock_guard<std::mutex> lock(frameStepMutex_);
  frameStepSerial_ = serial;
  clearFrameStepsLocked();
  frameStepResumeReanchorRequired_ = true;
}

StateChange Controller::requestFrameStep(
    playback_video_frame_step::Direction direction, int serial) {
  StateChange change = unchanged(current());
  if (change.current == PlayerState::Ended ||
      isActivePlaybackState(change.current)) {
    change = set(PlayerState::Paused);
  }
  if (!canClaimFrameStep(change.current)) {
    return change;
  }
  std::lock_guard<std::mutex> lock(frameStepMutex_);
  if (frameStepSerial_ != serial) {
    frameStepSerial_ = serial;
    clearFrameStepsLocked();
  }
  playback_video_frame_step::Request request;
  request.direction = direction;
  request.serial = serial;
  request.generation = nextFrameStepGeneration_++;
  frameStepRequests_.push_back(request);
  return change;
}

bool Controller::peekFrameStep(
    playback_video_frame_step::Request* request, int serial,
    bool frameStepSeekPending) {
  std::lock_guard<std::mutex> lock(frameStepMutex_);
  if (!canClaimFrameStep(current()) || frameStepSerial_ != serial) {
    frameStepSerial_ = serial;
    clearFrameStepsLocked();
    return false;
  }
  if (frameStepSeekPending || pendingSeekFrameStep_.active() ||
      frameStepRequests_.empty()) {
    return false;
  }
  *request = frameStepRequests_.front();
  return true;
}

bool Controller::discardFrameStep(
    const playback_video_frame_step::Request& request) {
  std::lock_guard<std::mutex> lock(frameStepMutex_);
  if (requestMatchesFrontLocked(request)) {
    frameStepRequests_.pop_front();
    return true;
  }
  return false;
}

bool Controller::publishFrameStepPresentation(
    const playback_video_frame_step::Request& request) {
  std::lock_guard<std::mutex> lock(frameStepMutex_);
  if (!requestMatchesFrontLocked(request)) {
    return false;
  }
  frameStepRequests_.pop_front();
  pendingPresentedFrameStep_.publish(request.serial, request.generation);
  return true;
}

FrameStepPresentationResult Controller::consumeFrameStepPresentation(
    int serial, uint64_t generation) {
  FrameStepPresentationResult result;
  std::lock_guard<std::mutex> lock(frameStepMutex_);
  if (!pendingPresentedFrameStep_.matches(serial, generation)) {
    result.change = unchanged(current());
    return result;
  }
  pendingPresentedFrameStep_.clear();
  PlayerState state = current();
  result.accepted = canClaimFrameStep(state);
  if (result.accepted) {
    frameStepResumeReanchorRequired_ = true;
  }
  result.change = result.accepted ? set(PlayerState::FrameStep)
                                  : unchanged(state);
  return result;
}

bool Controller::publishFrameStepSeek(
    const playback_video_frame_step::Request& request) {
  std::lock_guard<std::mutex> lock(frameStepMutex_);
  if (!requestMatchesFrontLocked(request)) {
    return false;
  }
  frameStepRequests_.pop_front();
  pendingSeekFrameStep_.publish(request.serial, request.generation);
  return true;
}

bool Controller::consumeFrameStepSeek(int serial, uint64_t generation) {
  std::lock_guard<std::mutex> lock(frameStepMutex_);
  if (!pendingSeekFrameStep_.matches(serial, generation)) {
    return false;
  }
  pendingSeekFrameStep_.clear();
  return canClaimFrameStep(current());
}

bool Controller::publishFrameStepSeekPresentation(int serial,
                                                  uint64_t generation) {
  std::lock_guard<std::mutex> lock(frameStepMutex_);
  if (frameStepSerial_ != serial || !canClaimFrameStep(current())) {
    return false;
  }
  pendingPresentedFrameStep_.publish(serial, generation);
  return true;
}

void Controller::cancelFrameStepSeek() {
  std::lock_guard<std::mutex> lock(frameStepMutex_);
  clearFrameStepsLocked();
}

FrameStepExitResult Controller::resumePlaybackFrameSteps() {
  FrameStepExitResult result;
  std::lock_guard<std::mutex> lock(frameStepMutex_);
  PlayerState state = current();
  result.requiresTimelineReanchor = frameStepResumeReanchorRequired_;
  frameStepResumeReanchorRequired_ = false;
  clearFrameStepsLocked();
  result.change = state == PlayerState::FrameStep ? set(PlayerState::Paused)
                                                   : unchanged(state);
  return result;
}

void Controller::clearFrameStepsLocked() {
  frameStepRequests_.clear();
  pendingPresentedFrameStep_.clear();
  pendingSeekFrameStep_.clear();
}

bool Controller::requestMatchesFrontLocked(
    const playback_video_frame_step::Request& request) const {
  if (frameStepRequests_.empty()) {
    return false;
  }
  const playback_video_frame_step::Request& front =
      frameStepRequests_.front();
  return request.serial == frameStepSerial_ && front.serial == request.serial &&
         front.generation == request.generation &&
         front.direction == request.direction;
}

}  // namespace playback_video_state_machine
