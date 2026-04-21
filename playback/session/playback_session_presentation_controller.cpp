#include "playback_session_presentation_controller.h"

#include "playback_mode.h"
#include "playback_session_output.h"
#include "playback_window_presentation.h"
#include "videowindow.h"

namespace {

void markPresentationChanged(bool& redraw, bool& forceRefreshArt) {
  forceRefreshArt = true;
  redraw = true;
}

PlaybackFullscreenToggleRequest fullscreenToggleRequest(
    PlaybackOutputController& output, bool enableAscii) {
  const VideoWindow& window = output.window();
  const bool windowOpen = window.IsOpen();
  const bool textGrid = windowOpen && window.IsTextGridPresentationEnabled();
  const bool terminalAscii = !output.windowActive() && enableAscii;

  PlaybackFullscreenToggleRequest request;
  request.family = (terminalAscii || textGrid)
                       ? PlaybackPresentationFamily::Ascii
                       : PlaybackPresentationFamily::Framebuffer;

  if (terminalAscii) {
    request.current = PlaybackPresentationMode::DefaultNonFullscreen;
  } else if (windowOpen && window.IsPictureInPicture()) {
    request.current = PlaybackPresentationMode::PictureInPicture;
  } else {
    request.current = PlaybackPresentationMode::Fullscreen;
  }
  return request;
}

}  // namespace

PlaybackPresentationController::PlaybackPresentationController(
    const PlaybackSessionContinuationState* continuationState) {
  if (continuationState) {
    pictureInPictureStartedFromTerminal =
        continuationState->windowPlacement.pictureInPictureStartedFromTerminal;
  }
}

void PlaybackPresentationController::clearPendingWindowPresentation() {
  pendingWindowPresentation = {};
}

void PlaybackPresentationController::requestWindowPresentation(
    PlaybackPresentationMode target, bool textGrid) {
  pendingWindowPresentation.active = true;
  pendingWindowPresentation.target = target;
  pendingWindowPresentation.textGrid = textGrid;
}

bool PlaybackPresentationController::toggleWindow(
    PlaybackOutputController& output, bool& redraw, bool& forceRefreshArt) {
  VideoWindow& window = output.window();
  if (window.IsOpen() && window.IsPictureInPicture()) {
    clearPendingWindowPresentation();
    playback_window_presentation::setTextGrid(
        window,
        !window.IsTextGridPresentationEnabled());
    output.requestWindowPresent();
    markPresentationChanged(redraw, forceRefreshArt);
    return true;
  }

  output.requestLayout(togglePlaybackLayout(output.desiredLayout()));
  markPresentationChanged(redraw, forceRefreshArt);
  if (isWindowPlaybackLayout(output.desiredLayout())) {
    output.requestWindowPresent();
  }
  return true;
}

bool PlaybackPresentationController::togglePictureInPicture(
    PlaybackOutputController& output, bool enableAscii, bool audioOnlyPlayback,
    bool& redraw, bool& forceRefreshArt) {
  VideoWindow& window = output.window();
  if (window.IsOpen() && window.IsPictureInPicture()) {
    const bool textGrid = window.IsTextGridPresentationEnabled();
    clearPendingWindowPresentation();
    if (pictureInPictureStartedFromTerminal) {
      pictureInPictureStartedFromTerminal = false;
      playback_window_presentation::setTextGrid(window, false);
      output.requestLayout(PlaybackLayout::Terminal);
      markPresentationChanged(redraw, forceRefreshArt);
      return true;
    }

    pictureInPictureStartedFromTerminal = false;
    markPresentationChanged(redraw, forceRefreshArt);
    output.requestWindowPresent();
    if (textGrid) {
      return playback_window_presentation::exitPictureInPicture(
          window, {PlaybackPresentationMode::Fullscreen, true});
    }
    return playback_window_presentation::exitPictureInPicture(
        window, {PlaybackPresentationMode::DefaultNonFullscreen, false});
  }

  const bool fromTerminalAscii = !output.windowActive() && enableAscii;
  const bool fromAsciiWindow =
      window.IsOpen() && window.IsTextGridPresentationEnabled();
  const bool textGrid =
      fromTerminalAscii || fromAsciiWindow || audioOnlyPlayback;
  requestWindowPresentation(PlaybackPresentationMode::PictureInPicture,
                            textGrid);
  pictureInPictureStartedFromTerminal = fromTerminalAscii;
  output.requestLayout(PlaybackLayout::Window);
  markPresentationChanged(redraw, forceRefreshArt);

  if (window.IsOpen()) {
    if (playback_window_presentation::apply(
            window, {PlaybackPresentationMode::PictureInPicture, textGrid})) {
      clearPendingWindowPresentation();
    }
  }
  output.requestWindowPresent();
  return true;
}

bool PlaybackPresentationController::toggleFullscreen(
    PlaybackOutputController& output, bool enableAscii, bool& redraw,
    bool& forceRefreshArt) {
  const PlaybackFullscreenToggleRequest request =
      fullscreenToggleRequest(output, enableAscii);
  const PlaybackFullscreenTogglePlan plan = planFullscreenToggle(request);
  VideoWindow& window = output.window();

  clearPendingWindowPresentation();
  pictureInPictureStartedFromTerminal = false;

  if (plan.target == PlaybackPresentationMode::Fullscreen) {
    output.requestLayout(PlaybackLayout::Window);
    markPresentationChanged(redraw, forceRefreshArt);
    requestWindowPresentation(PlaybackPresentationMode::Fullscreen,
                              request.family ==
                                  PlaybackPresentationFamily::Ascii);
    if (request.family == PlaybackPresentationFamily::Ascii) {
      if (window.IsOpen()) {
        if (playback_window_presentation::apply(
                window, {PlaybackPresentationMode::Fullscreen, true})) {
          clearPendingWindowPresentation();
        }
        output.requestWindowPresent();
      }
      return true;
    }

    if (window.IsOpen()) {
      if (playback_window_presentation::apply(
              window, {PlaybackPresentationMode::Fullscreen, false})) {
        clearPendingWindowPresentation();
      }
      output.requestWindowPresent();
    }
    return true;
  }

  if (request.family == PlaybackPresentationFamily::Ascii) {
    if (window.IsOpen()) {
      playback_window_presentation::setTextGrid(window, false);
    }
    output.requestLayout(PlaybackLayout::Terminal);
    markPresentationChanged(redraw, forceRefreshArt);
    return true;
  }

  requestWindowPresentation(PlaybackPresentationMode::PictureInPicture, false);
  output.requestLayout(PlaybackLayout::Window);
  markPresentationChanged(redraw, forceRefreshArt);
  if (window.IsOpen()) {
    if (playback_window_presentation::apply(
            window, {PlaybackPresentationMode::PictureInPicture, false})) {
      clearPendingWindowPresentation();
    }
    output.requestWindowPresent();
  }
  return true;
}

void PlaybackPresentationController::reconcile(
    PlaybackOutputController& output) {
  if (!output.windowRequested()) {
    clearPendingWindowPresentation();
    pictureInPictureStartedFromTerminal = false;
    return;
  }

  if (!pendingWindowPresentation.active || !output.window().IsOpen()) {
    return;
  }

  const bool applied = playback_window_presentation::apply(
      output.window(), {pendingWindowPresentation.target,
                        pendingWindowPresentation.textGrid});
  if (applied) {
    clearPendingWindowPresentation();
  }
  output.requestWindowPresent();
}

void PlaybackPresentationController::closePresentation(
    PlaybackOutputController& output, bool& redraw, bool& forceRefreshArt) {
  clearPendingWindowPresentation();
  pictureInPictureStartedFromTerminal = false;
  playback_window_presentation::setTextGrid(output.window(), false);
  output.requestLayout(PlaybackLayout::Terminal);
  markPresentationChanged(redraw, forceRefreshArt);
}

void PlaybackPresentationController::handleWindowClosed(
    PlaybackOutputController& output, bool& redraw, bool& forceRefreshArt) {
  closePresentation(output, redraw, forceRefreshArt);
}

void PlaybackPresentationController::captureWindowPlacement(
    PlaybackOutputController& output,
    PlaybackSessionContinuationState& state) const {
  playback_window_presentation::capturePlacement(
      output.window(), state.windowPlacement,
      pictureInPictureStartedFromTerminal);
}
