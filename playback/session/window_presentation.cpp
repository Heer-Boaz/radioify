#include "window_presentation.h"

#include <cassert>

#include "playback/video/framebuffer/window/window.h"

namespace {

VideoWindowFocus toVideoWindowFocus(PlaybackPresentationFocus focus) {
  switch (focus) {
    case PlaybackPresentationFocus::KeepCurrentSurface:
      return VideoWindowFocus::KeepCurrentFocus;
    case PlaybackPresentationFocus::FocusTargetSurface:
      return VideoWindowFocus::TakeForegroundFocus;
  }
  assert(false && "Unhandled playback presentation focus mode");
  return VideoWindowFocus::KeepCurrentFocus;
}

void setVisualMode(VideoWindow& window, PlaybackVisualMode visual) {
  window.SetTextGridPresentationEnabled(
      visual == PlaybackVisualMode::AsciiGrid);
}

}  // namespace

namespace playback_session_window {

bool apply(VideoWindow& window, WindowPresentationRequest request) {
  setVisualMode(window, request.visual);
  const VideoWindowFocus focus = toVideoWindowFocus(request.focus);

  switch (request.target) {
    case PlaybackWindowPresentationMode::Windowed:
      if (window.IsPictureInPicture() &&
          !window.SetPictureInPicture(false,
                                      VideoWindowFocus::KeepCurrentFocus)) {
        return false;
      }
      if (window.IsFullscreen() &&
          !window.SetFullscreen(false, VideoWindowFocus::KeepCurrentFocus)) {
        return false;
      }
      if (focus == VideoWindowFocus::TakeForegroundFocus) {
        window.Activate();
      }
      return true;
    case PlaybackWindowPresentationMode::Fullscreen:
      return window.IsPictureInPicture()
                 ? window.ExitPictureInPictureToFullscreen(focus)
                 : window.SetFullscreen(true, focus);
    case PlaybackWindowPresentationMode::PictureInPicture:
      return window.SetPictureInPicture(true, focus);
  }
  return false;
}

void applyPlacement(VideoWindow& window, const WindowPlacementState& state) {
  if (state.hasWindowedRect) {
    window.SetWindowBounds(state.windowedRect);
  }
}

void capturePlacement(const VideoWindow& window, WindowPlacementState& state,
                      const PlaybackPresentationState& presentation) {
  if (!window.IsOpen()) {
    return;
  }

  RECT windowedRect{};
  if (window.GetWindowedBounds(&windowedRect)) {
    state.hasWindowedRect = true;
    state.windowedRect = windowedRect;
  }

  if (presentation.layer() ==
      PlaybackPresentationLayer::PictureInPicture) {
    RECT pictureInPictureRect{};
    if (window.GetWindowBounds(&pictureInPictureRect)) {
      state.hasPictureInPictureRect = true;
      state.pictureInPictureRect = pictureInPictureRect;
    }
  }
}

}  // namespace playback_session_window
