#pragma once

#include "presentation_policy.h"
#include "state.h"

class PlaybackOutputController;

class PlaybackPresentationController {
 public:
  explicit PlaybackPresentationController(
      PlaybackPresentationState initialState =
          PlaybackPresentationState::terminalAscii());

  bool toggleWindow(PlaybackOutputController& output, bool& redraw,
                    bool& forceRefreshArt);
  bool togglePictureInPicture(PlaybackOutputController& output,
                              bool& redraw, bool& forceRefreshArt);
  bool toggleFullscreen(PlaybackOutputController& output, bool& redraw,
                        bool& forceRefreshArt);

  void closePresentation(PlaybackOutputController& output, bool& redraw,
                         bool& forceRefreshArt);
  void reconcile(PlaybackOutputController& output);
  void handleWindowClosed(PlaybackOutputController& output, bool& redraw,
                          bool& forceRefreshArt);
  void captureWindowPlacement(PlaybackOutputController& output,
                              PlaybackSessionContinuationState& state) const;

  const PlaybackPresentationState& state() const { return desiredState_; }
  bool usesAsciiGrid() const { return desiredState_.usesAsciiGrid(); }
  PlaybackShellTerminalRole terminalRole() const {
    return desiredState_.terminalRole();
  }

 private:
  bool transitionTo(PlaybackPresentationState next,
                    PlaybackOutputController& output, bool& redraw,
                    bool& forceRefreshArt);
  void queueWindowPresentation(PlaybackPresentationFocus focus);

  PlaybackPresentationState desiredState_;
  std::optional<PlaybackWindowPresentationRequest> pendingWindowPresentation_;
};
