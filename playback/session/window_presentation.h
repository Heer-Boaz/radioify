#pragma once

#include "presentation_policy.h"
#include "state.h"

class VideoWindow;

namespace playback_window_presentation {

struct WindowPresentationRequest {
  PlaybackPresentationMode target = PlaybackPresentationMode::Fullscreen;
  bool textGrid = false;
};

inline bool shouldStartFullscreen(const PlaybackWindowPlacementState& state) {
  return state.pictureInPictureActive
             ? state.pictureInPictureRestoreFullscreen
             : state.fullscreenActive;
}

bool apply(VideoWindow& window, WindowPresentationRequest request);

void setTextGrid(VideoWindow& window, bool enabled);

bool exitPictureInPicture(VideoWindow& window,
                          WindowPresentationRequest request);

void applyPlacement(VideoWindow& window,
                    const PlaybackWindowPlacementState& state);

void capturePlacement(const VideoWindow& window,
                      PlaybackWindowPlacementState& state,
                      bool pictureInPictureStartedFromTerminal);

}  // namespace playback_window_presentation
