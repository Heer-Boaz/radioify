#include "presentation_controller.h"

#include "output.h"
#include "playback_mode.h"

namespace {

void markPresentationChanged(bool& redraw, bool& forceRefreshArt) {
  forceRefreshArt = true;
  redraw = true;
}

PlaybackLayout layoutFor(const PlaybackPresentationState& state) {
  return state.requiresNativeWindow() ? PlaybackLayout::Window
                                      : PlaybackLayout::Terminal;
}

}  // namespace

PlaybackPresentationController::PlaybackPresentationController(
    PlaybackPresentationState initialState,
    WindowPlacementState initialPlacement)
    : desiredState_(initialState),
      appliedState_(initialState.requiresNativeWindow()
                        ? PlaybackPresentationState::terminalAscii()
                        : initialState),
      windowPlacement_(initialPlacement) {}

bool PlaybackPresentationController::transitionTo(
    PlaybackPresentationState next, PlaybackOutputController& output,
    bool& redraw, bool& forceRefreshArt) {
  if (next == desiredState_) {
    return true;
  }

  desiredState_ = next;
  output.requestLayout(layoutFor(desiredState_));
  markPresentationChanged(redraw, forceRefreshArt);

  if (!desiredState_.requiresNativeWindow()) {
    if (appliedState_.requiresNativeWindow() && output.windowOpen()) {
      output.captureWindowPlacement(windowPlacement_, appliedState_);
    }
    appliedState_ = desiredState_;
    return true;
  }

  if (output.windowOpen()) {
    const auto request = windowPresentationRequest(
        desiredState_, PlaybackPresentationFocus::FocusTargetSurface);
    if (!request || !output.applyWindowPresentation(*request)) {
      desiredState_ = appliedState_;
      output.requestLayout(layoutFor(desiredState_));
      markPresentationChanged(redraw, forceRefreshArt);
      return false;
    }
    appliedState_ = desiredState_;
    output.captureWindowPlacement(windowPlacement_, appliedState_);
  }
  return true;
}

bool PlaybackPresentationController::toggleWindow(
    PlaybackOutputController& output, bool& redraw, bool& forceRefreshArt) {
  return transitionTo(desiredState_.toggleWindowMode(), output, redraw,
                      forceRefreshArt);
}

bool PlaybackPresentationController::togglePictureInPicture(
    PlaybackOutputController& output, bool& redraw, bool& forceRefreshArt) {
  return transitionTo(desiredState_.togglePictureInPicture(), output, redraw,
                      forceRefreshArt);
}

bool PlaybackPresentationController::toggleFullscreen(
    PlaybackOutputController& output, bool& redraw, bool& forceRefreshArt) {
  return transitionTo(desiredState_.toggleFullscreen(), output, redraw,
                      forceRefreshArt);
}

void PlaybackPresentationController::reconcile(
    PlaybackOutputController& output, bool windowStartFailed, bool& redraw,
    bool& forceRefreshArt) {
  if (windowStartFailed) {
    desiredState_ = appliedState_;
    output.requestLayout(layoutFor(desiredState_));
    markPresentationChanged(redraw, forceRefreshArt);
    return;
  }

  if (!desiredState_.requiresNativeWindow()) {
    return;
  }

  if (desiredState_ == appliedState_ || !output.windowOpen()) {
    return;
  }

  const auto request = windowPresentationRequest(
      desiredState_, PlaybackPresentationFocus::FocusTargetSurface);
  const bool startingNativeWindow = !appliedState_.requiresNativeWindow();
  const bool applied =
      request &&
      (startingNativeWindow
           ? output.restoreWindowPresentation(*request, windowPlacement_)
           : output.applyWindowPresentation(*request));
  if (applied) {
    appliedState_ = desiredState_;
    output.captureWindowPlacement(windowPlacement_, appliedState_);
    return;
  }

  desiredState_ = appliedState_;
  output.requestLayout(layoutFor(desiredState_));
  markPresentationChanged(redraw, forceRefreshArt);
}

void PlaybackPresentationController::captureWindowPlacement(
    PlaybackOutputController& output,
    PlaybackSessionContinuationState& state) {
  if (appliedState_.requiresNativeWindow() && output.windowOpen()) {
    output.captureWindowPlacement(windowPlacement_, appliedState_);
  }
  state.hasPresentation = true;
  state.presentation = appliedState_;
  state.windowPlacement = windowPlacement_;
}
