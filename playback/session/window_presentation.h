#pragma once

#include "presentation_policy.h"
#include "state.h"

class VideoWindow;

namespace playback_session_window {

using WindowPresentationRequest = PlaybackWindowPresentationRequest;

bool apply(VideoWindow& window, WindowPresentationRequest request);

void setTextGrid(VideoWindow& window, bool enabled);

bool exitPictureInPicture(VideoWindow& window,
                          WindowPresentationRequest request);

void applyPlacement(VideoWindow& window,
                    const WindowPlacementState& state);

void capturePlacement(const VideoWindow& window,
                      WindowPlacementState& state,
                      bool pictureInPictureStartedFromTerminal);

}  // namespace playback_session_window
