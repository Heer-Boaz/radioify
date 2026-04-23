#include "state_machine.h"

namespace playback_state_machine {
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
  if (currentProjection.session != SessionState::Started) {
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
    } else if (currentState == PlayerState::Paused && !snapshot.audioPaused) {
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
                   (!audioActive || snapshot.audioFinished);
      if (ended) {
        nextState = PlayerState::Ended;
      } else if (snapshot.demuxEnded &&
                 currentState == PlayerState::Playing &&
                 snapshot.videoQueueEmpty) {
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
  transition.nextState =
      resolveSteadyState(currentState, snapshot, videoPrefillFrames);
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

}  // namespace playback_state_machine
