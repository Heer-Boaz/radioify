#pragma once

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "playback_mode.h"

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

enum class WindowThreadState : uint8_t {
  Disabled,
  Enabled,
  Stopping,
};

struct WindowPlacementState {
  bool hasWindowRect = false;
  RECT windowRect{};
  bool fullscreenActive = false;
  bool pictureInPictureActive = false;
  bool pictureInPictureRestoreFullscreen = false;
  bool textGridPresentationEnabled = false;
  bool pictureInPictureStartedFromTerminal = false;
  bool hasPictureInPictureRect = false;
  RECT pictureInPictureRect{};
  bool hasPictureInPictureRestoreRect = false;
  RECT pictureInPictureRestoreRect{};
};

struct PlaybackSessionContinuationState {
  bool hasLayout = false;
  PlaybackLayout layout = PlaybackLayout::Terminal;
  bool asciiRenderingEnabled = true;
  WindowPlacementState windowPlacement;
};
