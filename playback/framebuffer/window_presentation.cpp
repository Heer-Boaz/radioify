#include "window_presentation.h"

#include <cassert>

#include "playback/video/framebuffer/window/window.h"

namespace {

VideoWindowFocus toVideoWindowFocus(WindowFocusPolicy focus) {
  switch (focus) {
    case WindowFocusPolicy::PreserveCurrent:
      return VideoWindowFocus::KeepCurrentFocus;
    case WindowFocusPolicy::ActivateWindow:
      return VideoWindowFocus::TakeForegroundFocus;
  }
  assert(false && "Unhandled window focus policy");
  return VideoWindowFocus::KeepCurrentFocus;
}

void setContentMode(VideoWindow& window, WindowContentMode content) {
  window.SetTextGridPresentationEnabled(
      content == WindowContentMode::TextGrid);
}

bool matchesRequest(const VideoWindow& window,
                    const WindowPresentationRequest& request) {
  if (!window.IsOpen() || !window.IsVisible() ||
      window.IsTextGridPresentationEnabled() !=
          (request.content == WindowContentMode::TextGrid)) {
    return false;
  }
  switch (request.mode) {
    case WindowPresentationMode::Windowed:
      return !window.IsFullscreen() && !window.IsPictureInPicture();
    case WindowPresentationMode::Fullscreen:
      return window.IsFullscreen() && !window.IsPictureInPicture();
    case WindowPresentationMode::PictureInPicture:
      return !window.IsFullscreen() && window.IsPictureInPicture();
  }
  return false;
}

}  // namespace

namespace playback_window_presentation {

bool apply(VideoWindow& window, WindowPresentationRequest request) {
  const VideoWindowFocus focus = toVideoWindowFocus(request.focus);
  bool surfaceApplied = false;

  switch (request.mode) {
    case WindowPresentationMode::Windowed:
      if (!window.SetPictureInPicture(false,
                                      VideoWindowFocus::KeepCurrentFocus) ||
          !window.SetFullscreen(false,
                                VideoWindowFocus::KeepCurrentFocus)) {
        return false;
      }
      surfaceApplied = window.Show(focus);
      break;
    case WindowPresentationMode::Fullscreen:
      surfaceApplied = window.IsPictureInPicture()
                           ? window.ExitPictureInPictureToFullscreen(focus)
                           : window.SetFullscreen(true, focus);
      break;
    case WindowPresentationMode::PictureInPicture:
      surfaceApplied = window.SetPictureInPicture(true, focus);
      break;
  }
  if (!surfaceApplied) {
    return false;
  }
  setContentMode(window, request.content);
  return matchesRequest(window, request);
}

void applyPlacement(VideoWindow& window, const WindowPlacementState& state) {
  if (state.hasWindowedRect) {
    window.SetWindowBounds(state.windowedRect);
  }
}

void capturePlacement(const VideoWindow& window, WindowPlacementState& state) {
  if (!window.IsOpen()) {
    return;
  }

  RECT windowedRect{};
  if (window.GetWindowedBounds(&windowedRect)) {
    state.hasWindowedRect = true;
    state.windowedRect = windowedRect;
  }

  if (window.IsPictureInPicture()) {
    RECT pictureInPictureRect{};
    if (window.GetWindowBounds(&pictureInPictureRect)) {
      state.hasPictureInPictureRect = true;
      state.pictureInPictureRect = pictureInPictureRect;
    }
  }
}

}  // namespace playback_window_presentation
