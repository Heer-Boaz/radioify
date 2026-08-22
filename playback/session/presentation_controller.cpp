#include "presentation_controller.h"

#include "output.h"

PlaybackPresentationController::PlaybackPresentationController(
    PlaybackPresentationState initialState,
    WindowPlacementState initialPlacement)
    : desiredState_(initialState),
      pendingFocus_(presentationFocusFor(initialState)),
      windowPlacement_(initialPlacement) {}

bool PlaybackPresentationController::transitionTo(
    PlaybackPresentationState next, WindowFocusPolicy focus) {
  if (next == desiredState_) {
    return false;
  }
  desiredState_ = next;
  pendingFocus_ = focus;
  return true;
}

bool PlaybackPresentationController::toggleWindow() {
  const PlaybackPresentationState next = desiredState_.toggleWindowMode();
  return transitionTo(next, presentationFocusFor(next));
}

bool PlaybackPresentationController::togglePictureInPicture() {
  const PlaybackPresentationState next =
      desiredState_.togglePictureInPicture();
  return transitionTo(next, presentationFocusFor(next));
}

bool PlaybackPresentationController::toggleFullscreen() {
  const PlaybackPresentationState next = desiredState_.toggleFullscreen();
  return transitionTo(next, presentationFocusFor(next));
}

PlaybackPresentationSyncResult
PlaybackPresentationController::synchronize(
    PlaybackOutputController& output) {
  PlaybackPresentationSyncResult result;
  result.previousState = state();
  result.previousWindowOpen = output.windowOpen();

  const auto finish =
      [this, &result, &output](
          bool failed,
          std::optional<PlaybackShellFocusTarget> focusTarget) {
        result.appliedState = state();
        result.windowOpen = output.windowOpen();
        result.transitionFailed = failed;
        result.shellFocusTarget = focusTarget;
        pendingFocus_ = WindowFocusPolicy::PreserveCurrent;
        return result;
      };

  const bool statePending =
      !appliedState_ || desiredState_ != *appliedState_;
  const bool physicalStateMatches =
      desiredState_.requiresNativeWindow() == output.windowOpen();
  if (!statePending && physicalStateMatches) {
    return finish(false, std::nullopt);
  }

  if (!desiredState_.requiresNativeWindow()) {
    if (output.windowOpen()) {
      if (appliedState_ && appliedState_->requiresNativeWindow()) {
        output.captureWindowPlacement(windowPlacement_);
      }
      output.closeWindow();
    }
    appliedState_ = desiredState_;
    std::optional<PlaybackShellFocusTarget> focusTarget =
        shellFocusAfterTransition(result.previousState, *appliedState_);
    if (!focusTarget && result.previousWindowOpen &&
        pendingFocus_ == WindowFocusPolicy::ActivateWindow) {
      focusTarget = PlaybackShellFocusTarget::TerminalPlayback;
    }
    return finish(false, focusTarget);
  }

  const std::optional<PlaybackPresentationState> previousApplied =
      appliedState_;
  const bool openedWindow = !output.windowOpen();
  if (openedWindow && !output.openWindow()) {
    desiredState_ = PlaybackPresentationState::terminalAscii();
    appliedState_ = desiredState_;
    return finish(true, PlaybackShellFocusTarget::TerminalPlayback);
  }

  const auto request = windowPresentationRequest(desiredState_, pendingFocus_);
  const bool applied =
      request &&
      (openedWindow
           ? output.restoreWindowPresentation(*request, windowPlacement_)
           : output.applyWindowPresentation(*request));
  if (applied) {
    appliedState_ = desiredState_;
    output.captureWindowPlacement(windowPlacement_);
    return finish(false, shellFocusAfterTransition(result.previousState,
                                                    *appliedState_));
  }

  const bool canRestorePreviousWindowState =
      !openedWindow && previousApplied &&
      previousApplied->requiresNativeWindow() && output.windowOpen();
  if (canRestorePreviousWindowState) {
    const auto rollback = windowPresentationRequest(
        *previousApplied, WindowFocusPolicy::PreserveCurrent);
    if (rollback && output.applyWindowPresentation(*rollback)) {
      desiredState_ = *previousApplied;
      appliedState_ = *previousApplied;
      return finish(true, std::nullopt);
    }
  }

  output.closeWindow();
  desiredState_ = PlaybackPresentationState::terminalAscii();
  appliedState_ = desiredState_;
  return finish(true, PlaybackShellFocusTarget::TerminalPlayback);
}

void PlaybackPresentationController::captureWindowPlacement(
    PlaybackOutputController& output,
    PlaybackSessionContinuationState& state) {
  const PlaybackPresentationState& applied = this->state();
  if (applied.requiresNativeWindow() && output.windowOpen()) {
    output.captureWindowPlacement(windowPlacement_);
  }
  state.presentation = applied;
  state.windowPlacement = windowPlacement_;
}
