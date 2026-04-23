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

enum class WindowThreadState : uint8_t {
  Disabled,
  Enabled,
  Stopping,
};

struct PlaybackWindowPlacementState {
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
  PlaybackWindowPlacementState windowPlacement;
};
