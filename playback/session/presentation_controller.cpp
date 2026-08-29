#include "presentation_controller.h"

namespace {

bool isOpen(PlaybackWindowLifecycle lifecycle) {
  return lifecycle == PlaybackWindowLifecycle::Open;
}

}  // namespace

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
    PlaybackPresentationBackend& output) {
  (void)output.consumeWindowLifecycleChange();

  PlaybackPresentationSyncResult result;
  result.previousState = state();
  result.previousWindowOpen = isOpen(output.windowLifecycle());

  const auto finish =
      [this, &result, &output](
          bool pending, bool failed,
          std::optional<PlaybackShellFocusTarget> focusTarget) {
        result.appliedState = state();
        result.windowOpen = isOpen(output.windowLifecycle());
        result.transitionPending = pending;
        result.transitionFailed = failed;
        result.shellFocusTarget = focusTarget;
        if (!pending) {
          pendingFocus_ = WindowFocusPolicy::PreserveCurrent;
          transitionFailurePending_ = false;
        }
        return result;
      };

  const auto pending = [&]() {
    return finish(true, false, std::nullopt);
  };

  const auto completeTerminal = [&]() {
    appliedState_ = desiredState_;
    std::optional<PlaybackShellFocusTarget> focusTarget =
        shellFocusAfterTransition(result.previousState, appliedState_);
    if (!focusTarget && result.previousState.requiresNativeWindow() &&
        pendingFocus_ == WindowFocusPolicy::ActivateWindow) {
      focusTarget = PlaybackShellFocusTarget::TerminalPlayback;
    }
    return finish(false, transitionFailurePending_, focusTarget);
  };

  const bool statePending = desiredState_ != appliedState_;
  PlaybackWindowLifecycle lifecycle = output.windowLifecycle();
  const bool physicalStateMatches =
      desiredState_.requiresNativeWindow()
          ? lifecycle == PlaybackWindowLifecycle::Open
          : lifecycle == PlaybackWindowLifecycle::Closed;
  if (!statePending && physicalStateMatches) {
    return finish(false, false, std::nullopt);
  }

  if (!desiredState_.requiresNativeWindow()) {
    if (lifecycle != PlaybackWindowLifecycle::Closed) {
      if (lifecycle == PlaybackWindowLifecycle::Open &&
          appliedState_.requiresNativeWindow()) {
        output.captureWindowPlacement(windowPlacement_);
      }
      if (lifecycle != PlaybackWindowLifecycle::Closing) {
        output.requestCloseWindow();
      }
      if (!output.windowCloseReady() || !output.finishCloseWindow()) {
        return pending();
      }
    }
    return completeTerminal();
  }

  const PlaybackPresentationState previousApplied = appliedState_;

  if (lifecycle == PlaybackWindowLifecycle::Closing) {
    if (!output.windowCloseReady() || !output.finishCloseWindow()) {
      return pending();
    }
    lifecycle = PlaybackWindowLifecycle::Closed;
  }

  if (lifecycle == PlaybackWindowLifecycle::Failed) {
    transitionFailurePending_ = true;
    desiredState_ = PlaybackPresentationState::terminalAscii();
    output.requestCloseWindow();
    if (!output.windowCloseReady() || !output.finishCloseWindow()) {
      return pending();
    }
    return completeTerminal();
  }

  if (lifecycle == PlaybackWindowLifecycle::Closed) {
    if (!output.requestOpenWindow()) {
      transitionFailurePending_ = true;
      desiredState_ = PlaybackPresentationState::terminalAscii();
      output.requestCloseWindow();
      if (!output.windowCloseReady() || !output.finishCloseWindow()) {
        return pending();
      }
      return completeTerminal();
    }
    return pending();
  }

  if (lifecycle == PlaybackWindowLifecycle::Opening) {
    return pending();
  }

  const bool openedWindow =
      !previousApplied.requiresNativeWindow();
  const auto request = windowPresentationRequest(desiredState_, pendingFocus_);
  const bool applied =
      request &&
      (openedWindow
           ? output.restoreWindowPresentation(*request, windowPlacement_)
           : output.applyWindowPresentation(*request));
  if (applied) {
    appliedState_ = desiredState_;
    output.captureWindowPlacement(windowPlacement_);
    return finish(false, false,
                  shellFocusAfterTransition(result.previousState,
                                            appliedState_));
  }

  const bool canRestorePreviousWindowState =
      !openedWindow && previousApplied.requiresNativeWindow() &&
      output.windowLifecycle() == PlaybackWindowLifecycle::Open;
  if (canRestorePreviousWindowState) {
    const auto rollback = windowPresentationRequest(
        previousApplied, WindowFocusPolicy::PreserveCurrent);
    if (rollback && output.applyWindowPresentation(*rollback)) {
      desiredState_ = previousApplied;
      appliedState_ = previousApplied;
      return finish(false, true, std::nullopt);
    }
  }

  transitionFailurePending_ = true;
  desiredState_ = PlaybackPresentationState::terminalAscii();
  output.requestCloseWindow();
  if (!output.windowCloseReady() || !output.finishCloseWindow()) {
    return pending();
  }
  return completeTerminal();
}

void PlaybackPresentationController::captureWindowPlacement(
    PlaybackPresentationBackend& output,
    PlaybackSessionContinuationState& state) {
  const PlaybackPresentationState& applied = this->state();
  if (applied.requiresNativeWindow() &&
      output.windowLifecycle() == PlaybackWindowLifecycle::Open) {
    output.captureWindowPlacement(windowPlacement_);
  }
  state.presentation = applied;
  state.windowPlacement = windowPlacement_;
}
