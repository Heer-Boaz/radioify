#pragma once

#include <functional>
#include <optional>

#include "playback/framebuffer/presenter.h"
#include "presentation_policy.h"
#include "state.h"

class PlaybackOutputController;
class Player;
struct WindowUiState;

struct PlaybackPresentationSyncResult {
  PlaybackPresentationState previousState =
      PlaybackPresentationState::terminalAscii();
  PlaybackPresentationState appliedState =
      PlaybackPresentationState::terminalAscii();
  bool previousWindowOpen = false;
  bool windowOpen = false;
  bool transitionFailed = false;
  std::optional<PlaybackShellFocusTarget> shellFocusTarget;

  bool switchedAwayFromWindow() const {
    return previousWindowOpen && !windowOpen;
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
      PlaybackOutputController& output, Player& player,
      const std::function<WindowUiState()>& buildUiState,
      const playback_framebuffer_presenter::TextGridPresentationProvider&
          buildTextGridPresentation);
  void captureWindowPlacement(PlaybackOutputController& output,
                              PlaybackSessionContinuationState& state);

  const PlaybackPresentationState& state() const {
    return appliedState_ ? *appliedState_ : desiredState_;
  }
  PlaybackShellTerminalRole terminalRole() const {
    return state().terminalRole();
  }

 private:
  bool transitionTo(PlaybackPresentationState next,
                    WindowFocusPolicy focus);

  PlaybackPresentationState desiredState_;
  std::optional<PlaybackPresentationState> appliedState_;
  WindowFocusPolicy pendingFocus_ = WindowFocusPolicy::ActivateWindow;
  WindowPlacementState windowPlacement_;
};
