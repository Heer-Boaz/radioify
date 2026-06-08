#include "window_presentation.h"

#include "playback/video/framebuffer/window/window.h"

#include <cassert>

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

}  // namespace

namespace playback_session_window {

void setTextGrid(VideoWindow& window, bool enabled) {
  window.SetTextGridPresentationEnabled(enabled);
}

bool exitPictureInPicture(VideoWindow& window,
                          WindowPresentationRequest request) {
  setTextGrid(window, request.textGrid);
  const VideoWindowFocus focus = toVideoWindowFocus(request.focus);
  switch (request.target) {
    case PlaybackPresentationMode::Fullscreen:
      return window.ExitPictureInPictureToFullscreen(focus);
    case PlaybackPresentationMode::DefaultNonFullscreen:
      return window.SetPictureInPicture(false, focus);
    case PlaybackPresentationMode::PictureInPicture:
      return true;
  }
  return false;
}

bool apply(VideoWindow& window, WindowPresentationRequest request) {
  setTextGrid(window, request.textGrid);
  const VideoWindowFocus focus = toVideoWindowFocus(request.focus);
  switch (request.target) {
    case PlaybackPresentationMode::Fullscreen:
      return window.IsPictureInPicture()
                 ? exitPictureInPicture(window, request)
                 : window.SetFullscreen(true, focus);
    case PlaybackPresentationMode::PictureInPicture:
      return window.SetPictureInPicture(true, focus);
    case PlaybackPresentationMode::DefaultNonFullscreen:
      return true;
  }
  return false;
}

void applyPlacement(VideoWindow& window,
                    const WindowPlacementState& state) {
  setTextGrid(window, state.textGridPresentationEnabled);

  if (state.pictureInPictureActive) {
    if (state.pictureInPictureRestoreFullscreen) {
      apply(window, restoredWindowPresentation(
                        PlaybackPresentationMode::Fullscreen,
                        state.textGridPresentationEnabled));
    } else if (state.hasPictureInPictureRestoreRect) {
      window.SetWindowBounds(state.pictureInPictureRestoreRect);
    } else if (state.hasWindowRect) {
      window.SetWindowBounds(state.windowRect);
    }

    if (window.SetPictureInPicture(true, VideoWindowFocus::KeepCurrentFocus) &&
        state.hasPictureInPictureRect) {
      window.SetWindowBounds(state.pictureInPictureRect);
    }
    return;
  }

  if (state.fullscreenActive) {
    apply(window, restoredWindowPresentation(
                      PlaybackPresentationMode::Fullscreen,
                      state.textGridPresentationEnabled));
    return;
  }

  if (state.hasWindowRect) {
    window.SetWindowBounds(state.windowRect);
  }
}

void capturePlacement(const VideoWindow& window,
                      WindowPlacementState& state,
                      bool pictureInPictureStartedFromTerminal) {
  state.pictureInPictureStartedFromTerminal =
      pictureInPictureStartedFromTerminal;
  if (!window.IsOpen()) {
    return;
  }

  RECT windowRect{};
  if (window.GetWindowBounds(&windowRect)) {
    state.hasWindowRect = true;
    state.windowRect = windowRect;
  }

  state.pictureInPictureActive = window.IsPictureInPicture();
  state.fullscreenActive = window.IsFullscreen();
  state.textGridPresentationEnabled = window.IsTextGridPresentationEnabled();
  if (!state.pictureInPictureActive) {
    return;
  }

  state.pictureInPictureRestoreFullscreen =
      window.PictureInPictureRestoresFullscreen();
  if (window.GetPictureInPictureRestoreBounds(
          &state.pictureInPictureRestoreRect)) {
    state.hasPictureInPictureRestoreRect = true;
  }
  if (window.GetWindowBounds(&state.pictureInPictureRect)) {
    state.hasPictureInPictureRect = true;
  }
}

}  // namespace playback_session_window
