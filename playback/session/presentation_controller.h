#pragma once

#include "presentation_policy.h"
#include "state.h"

class PlaybackOutputController;

class PlaybackPresentationController {
 public:
  explicit PlaybackPresentationController(
      PlaybackPresentationState initialState =
          PlaybackPresentationState::terminalAscii(),
      WindowPlacementState initialPlacement = {});

  bool toggleWindow(PlaybackOutputController& output, bool& redraw,
                    bool& forceRefreshArt);
  bool togglePictureInPicture(PlaybackOutputController& output,
                              bool& redraw, bool& forceRefreshArt);
  bool toggleFullscreen(PlaybackOutputController& output, bool& redraw,
                        bool& forceRefreshArt);

  void reconcile(PlaybackOutputController& output, bool windowStartFailed,
                 bool& redraw, bool& forceRefreshArt);
  void captureWindowPlacement(PlaybackOutputController& output,
                              PlaybackSessionContinuationState& state);

  const PlaybackPresentationState& state() const { return desiredState_; }
  bool usesAsciiGrid() const { return desiredState_.usesAsciiGrid(); }
  PlaybackShellTerminalRole terminalRole() const {
    return desiredState_.terminalRole();
  }

 private:
  bool transitionTo(PlaybackPresentationState next,
                    PlaybackOutputController& output, bool& redraw,
                    bool& forceRefreshArt);

  PlaybackPresentationState desiredState_;
  PlaybackPresentationState appliedState_;
  WindowPlacementState windowPlacement_;
};
