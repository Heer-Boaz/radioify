#include "presentation_controller.h"

#include "output.h"
#include "playback_mode.h"
#include "window_presentation.h"

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
    PlaybackPresentationState initialState)
    : desiredState_(initialState) {
  queueWindowPresentation(PlaybackPresentationFocus::KeepCurrentSurface);
}

void PlaybackPresentationController::queueWindowPresentation(
    PlaybackPresentationFocus focus) {
  pendingWindowPresentation_ =
      windowPresentationRequest(desiredState_, focus);
}

bool PlaybackPresentationController::transitionTo(
    PlaybackPresentationState next, PlaybackOutputController& output,
    bool& redraw, bool& forceRefreshArt) {
  if (next == desiredState_) {
    return true;
  }

  desiredState_ = next;
  queueWindowPresentation(PlaybackPresentationFocus::FocusTargetSurface);
  output.requestLayout(layoutFor(desiredState_));
  markPresentationChanged(redraw, forceRefreshArt);

  if (output.windowOpen() && pendingWindowPresentation_) {
    if (playback_session_window::apply(output.window(),
                                       *pendingWindowPresentation_)) {
      pendingWindowPresentation_.reset();
    }
  }
  if (desiredState_.requiresNativeWindow()) {
    output.requestWindowPresent();
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
    PlaybackOutputController& output) {
  if (!desiredState_.requiresNativeWindow()) {
    pendingWindowPresentation_.reset();
    return;
  }

  if (!pendingWindowPresentation_ || !output.windowOpen()) {
    return;
  }

  if (playback_session_window::apply(output.window(),
                                     *pendingWindowPresentation_)) {
    pendingWindowPresentation_.reset();
  }
  output.requestWindowPresent();
}

void PlaybackPresentationController::closePresentation(
    PlaybackOutputController& output, bool& redraw, bool& forceRefreshArt) {
  transitionTo(PlaybackPresentationState::terminalAscii(), output, redraw,
               forceRefreshArt);
}

void PlaybackPresentationController::handleWindowClosed(
    PlaybackOutputController& output, bool& redraw, bool& forceRefreshArt) {
  closePresentation(output, redraw, forceRefreshArt);
}

void PlaybackPresentationController::captureWindowPlacement(
    PlaybackOutputController& output,
    PlaybackSessionContinuationState& state) const {
  state.hasPresentation = true;
  state.presentation = desiredState_;
  playback_session_window::capturePlacement(
      output.window(), state.windowPlacement, desiredState_);
}
