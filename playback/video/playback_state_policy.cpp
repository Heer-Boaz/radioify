#include "playback_state_policy.h"

namespace playback_state_policy {
namespace {

bool isActivePlaybackState(PlayerState state) {
  return state == PlayerState::Playing || state == PlayerState::Prefill ||
         state == PlayerState::Draining || state == PlayerState::Priming;
}

}  // namespace

size_t requiredAudioPrefillFrames(uint32_t sampleRate) {
  uint32_t effectiveSampleRate = sampleRate > 0 ? sampleRate : 48000;
  return static_cast<size_t>(effectiveSampleRate / 3);
}

bool isPrefillReady(const Snapshot& snapshot, size_t videoPrefillFrames) {
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

bool shouldHoldAudioOutput(PlayerState state) {
  return state == PlayerState::Opening || state == PlayerState::Prefill ||
         state == PlayerState::Priming || state == PlayerState::Seeking;
}

bool shouldPauseMainClock(PlayerState state) {
  return state == PlayerState::Idle || state == PlayerState::Opening ||
         state == PlayerState::Prefill || state == PlayerState::Paused ||
         state == PlayerState::Ended || state == PlayerState::Error ||
         state == PlayerState::Closing;
}

PlayerState resolveSteadyState(Snapshot snapshot, size_t videoPrefillFrames) {
  while (true) {
    PlayerState nextState = snapshot.currentState;

    if (snapshot.currentState == PlayerState::Opening && snapshot.initDone) {
      nextState = snapshot.initOk
                      ? (snapshot.audioPaused ? PlayerState::Paused
                                              : PlayerState::Prefill)
                      : PlayerState::Error;
    } else if (snapshot.currentState == PlayerState::Seeking &&
               snapshot.seekInFlightSerial == 0) {
      if (snapshot.seekFailed) {
        nextState =
            snapshot.audioPaused ? PlayerState::Paused : PlayerState::Prefill;
      } else if (snapshot.audioPaused &&
                 snapshot.pendingSeekSerial == snapshot.currentSerial &&
                 !(snapshot.decodeEnded && snapshot.videoQueueEmpty)) {
        // A paused seek borrows Seeking only long enough to display one frame.
        nextState = PlayerState::Seeking;
      } else {
        nextState =
            snapshot.audioPaused ? PlayerState::Paused : PlayerState::Prefill;
      }
    } else if (isActivePlaybackState(snapshot.currentState) &&
               snapshot.audioPaused) {
      nextState = PlayerState::Paused;
    } else if (snapshot.currentState == PlayerState::Paused &&
               !snapshot.audioPaused) {
      nextState = snapshot.lastPresentedSerial == snapshot.currentSerial
                      ? PlayerState::Playing
                      : PlayerState::Prefill;
    } else if (snapshot.currentState == PlayerState::Prefill &&
               !snapshot.audioPaused &&
               isPrefillReady(snapshot, videoPrefillFrames)) {
      nextState = PlayerState::Priming;
    } else if (isActivePlaybackState(snapshot.currentState)) {
      bool audioActive = snapshot.audioStartedOk && !snapshot.audioFinished;
      bool ended = snapshot.decodeEnded && snapshot.videoQueueEmpty &&
                   (!audioActive || snapshot.audioFinished);
      if (ended) {
        nextState = PlayerState::Ended;
      } else if (snapshot.demuxEnded &&
                 snapshot.currentState == PlayerState::Playing &&
                 snapshot.videoQueueEmpty) {
        nextState = PlayerState::Draining;
      }
    }

    if (nextState == snapshot.currentState) {
      return nextState;
    }
    snapshot.currentState = nextState;
  }
}

}  // namespace playback_state_policy
