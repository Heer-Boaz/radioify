#pragma once

#include <optional>

#include "presentation_backend.h"
#include "presentation_policy.h"
#include "state.h"

struct PlaybackPresentationSyncResult {
  PlaybackPresentationState previousState =
      PlaybackPresentationState::terminalAscii();
  PlaybackPresentationState appliedState =
      PlaybackPresentationState::terminalAscii();
  bool previousWindowOpen = false;
  bool windowOpen = false;
  bool transitionPending = false;
  bool transitionFailed = false;
  std::optional<PlaybackShellFocusTarget> shellFocusTarget;

  bool switchedAwayFromWindow() const {
    return previousState.requiresNativeWindow() &&
           !appliedState.requiresNativeWindow();
  }

  bool visualModeChanged() const {
    return previousState.visual() != appliedState.visual();
  }
};

class PlaybackPresentationController {
 public:
  explicit PlaybackPresentationController(
      PlaybackPresentationState initialState =
          PlaybackPresentationState::terminalAscii(),
      WindowPlacementState initialPlacement = {});

  bool toggleWindow();
  bool togglePictureInPicture();
  bool toggleFullscreen();

  PlaybackPresentationSyncResult synchronize(
      PlaybackPresentationBackend& output);
  void captureWindowPlacement(PlaybackPresentationBackend& output,
                              PlaybackSessionContinuationState& state);

  const PlaybackPresentationState& state() const {
    return appliedState_;
  }
  PlaybackShellTerminalRole terminalRole() const {
    return state().terminalRole();
  }

 private:
  bool transitionTo(PlaybackPresentationState next,
                    WindowFocusPolicy focus);

  PlaybackPresentationState desiredState_;
  PlaybackPresentationState appliedState_ =
      PlaybackPresentationState::terminalAscii();
  WindowFocusPolicy pendingFocus_ = WindowFocusPolicy::ActivateWindow;
  WindowPlacementState windowPlacement_;
  bool transitionFailurePending_ = false;
};
