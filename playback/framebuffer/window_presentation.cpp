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
  setContentMode(window, request.content);

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
  return matchesRequest(window, request);
}

bool restore(VideoWindow& window, WindowPresentationRequest request,
             const WindowPlacementState& state) {
  if (!state.hasWindowedPlacement) {
    if (!apply(window, request)) {
      return false;
    }
  } else {
    const VideoWindowedPlacement placement{
        state.windowedNormalRect, state.windowedMaximized};
    const VideoWindowFocus focus = toVideoWindowFocus(request.focus);
    setContentMode(window, request.content);

    bool restored = false;
    switch (request.mode) {
      case WindowPresentationMode::Windowed:
        restored = window.RestoreWindowed(placement, focus);
        break;
      case WindowPresentationMode::Fullscreen:
        restored = window.RestoreFullscreen(placement, focus);
        break;
      case WindowPresentationMode::PictureInPicture:
        restored = window.RestorePictureInPicture(placement, focus);
        break;
    }
    if (!restored || !matchesRequest(window, request)) {
      return false;
    }
  }

  return request.mode != WindowPresentationMode::PictureInPicture ||
         !state.hasPictureInPictureRect ||
         window.SetWindowBounds(state.pictureInPictureRect);
}

void capturePlacement(const VideoWindow& window, WindowPlacementState& state) {
  if (!window.IsOpen()) {
    return;
  }

  VideoWindowedPlacement windowedPlacement;
  if (window.GetWindowedPlacement(&windowedPlacement)) {
    state.hasWindowedPlacement = true;
    state.windowedNormalRect = windowedPlacement.normalBounds;
    state.windowedMaximized = windowedPlacement.maximized;
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
