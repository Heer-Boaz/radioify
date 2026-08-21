#pragma once

#include <cstdint>

#include "presentation_policy.h"

enum class PlaybackSessionState : uint8_t {
  Active,
  Paused,
  Ended,
  Exiting,
};

// Describes why a new session was opened. It is an application intent, not a
// rendering option: presentation can still switch freely between terminal,
// framebuffer, fullscreen, and PiP while the session is running.
enum class PlaybackSessionIntent : uint8_t {
  View,
  EditVideo,
};

namespace playback_session_state {

inline bool toggleRequestsPause(PlaybackSessionState sessionState,
                                bool playerEnded) {
  return sessionState == PlaybackSessionState::Active && !playerEnded;
}

}  // namespace playback_session_state

struct PlaybackSessionContinuationState {
  bool hasPresentation = false;
  PlaybackPresentationState presentation =
      PlaybackPresentationState::terminalAscii();
  WindowPlacementState windowPlacement;
};
