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

bool matchesRequest(const VideoWindow& window,
                    const PlaybackWindowPresentationRequest& request) {
  if (!window.IsOpen() || !window.IsVisible() ||
      window.IsTextGridPresentationEnabled() !=
          (request.visual == PlaybackVisualMode::AsciiGrid)) {
    return false;
  }
  switch (request.target) {
    case PlaybackWindowPresentationMode::Windowed:
      return !window.IsFullscreen() && !window.IsPictureInPicture();
    case PlaybackWindowPresentationMode::Fullscreen:
      return window.IsFullscreen() && !window.IsPictureInPicture();
    case PlaybackWindowPresentationMode::PictureInPicture:
      return !window.IsFullscreen() && window.IsPictureInPicture();
  }
  return false;
}

}  // namespace

namespace playback_session_window {

bool apply(VideoWindow& window, WindowPresentationRequest request) {
  const VideoWindowFocus focus = toVideoWindowFocus(request.focus);
  bool surfaceApplied = false;

  switch (request.target) {
    case PlaybackWindowPresentationMode::Windowed:
      if (!window.SetPictureInPicture(false,
                                      VideoWindowFocus::KeepCurrentFocus) ||
          !window.SetFullscreen(false,
                                VideoWindowFocus::KeepCurrentFocus)) {
        return false;
      }
      surfaceApplied = window.Show(focus);
      break;
    case PlaybackWindowPresentationMode::Fullscreen:
      surfaceApplied = window.IsPictureInPicture()
                           ? window.ExitPictureInPictureToFullscreen(focus)
                           : window.SetFullscreen(true, focus);
      break;
    case PlaybackWindowPresentationMode::PictureInPicture:
      surfaceApplied = window.SetPictureInPicture(true, focus);
      break;
  }
  if (!surfaceApplied) {
    return false;
  }
  setVisualMode(window, request.visual);
  return matchesRequest(window, request);
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
