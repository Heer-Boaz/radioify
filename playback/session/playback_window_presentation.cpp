#include "playback_window_presentation.h"

#include "videowindow.h"

namespace playback_window_presentation {

void setTextGrid(VideoWindow& window, bool enabled) {
  window.SetTextGridPresentationEnabled(enabled);
}

bool exitPictureInPicture(VideoWindow& window,
                          WindowPresentationRequest request) {
  setTextGrid(window, request.textGrid);
  switch (request.target) {
    case PlaybackPresentationMode::Fullscreen:
      return window.ExitPictureInPictureToFullscreen();
    case PlaybackPresentationMode::DefaultNonFullscreen:
      return window.SetPictureInPicture(false);
    case PlaybackPresentationMode::PictureInPicture:
      return true;
  }
  return false;
}

bool apply(VideoWindow& window, WindowPresentationRequest request) {
  setTextGrid(window, request.textGrid);
  switch (request.target) {
    case PlaybackPresentationMode::Fullscreen:
      return window.IsPictureInPicture()
                 ? exitPictureInPicture(window, request)
                 : window.SetFullscreen(true);
    case PlaybackPresentationMode::PictureInPicture:
      return window.SetPictureInPicture(true);
    case PlaybackPresentationMode::DefaultNonFullscreen:
      return true;
  }
  return false;
}

void applyPlacement(VideoWindow& window,
                    const PlaybackWindowPlacementState& state) {
  setTextGrid(window, state.textGridPresentationEnabled);

  if (state.pictureInPictureActive) {
    if (state.pictureInPictureRestoreFullscreen) {
      apply(window, {PlaybackPresentationMode::Fullscreen,
                     state.textGridPresentationEnabled});
    } else if (state.hasPictureInPictureRestoreRect) {
      window.SetWindowBounds(state.pictureInPictureRestoreRect);
    } else if (state.hasWindowRect) {
      window.SetWindowBounds(state.windowRect);
    }

    if (window.SetPictureInPicture(true) && state.hasPictureInPictureRect) {
      window.SetWindowBounds(state.pictureInPictureRect);
    }
    return;
  }

  if (state.fullscreenActive) {
    apply(window, {PlaybackPresentationMode::Fullscreen,
                   state.textGridPresentationEnabled});
    return;
  }

  if (state.hasWindowRect) {
    window.SetWindowBounds(state.windowRect);
  }
}

void capturePlacement(const VideoWindow& window,
                      PlaybackWindowPlacementState& state,
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

}  // namespace playback_window_presentation
