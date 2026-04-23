#pragma once

#include "presentation_policy.h"
#include "state.h"

class PlaybackOutputController;

class PlaybackPresentationController {
 public:
  explicit PlaybackPresentationController(
      const PlaybackSessionContinuationState* continuationState = nullptr);

  bool toggleWindow(PlaybackOutputController& output, bool& redraw,
                    bool& forceRefreshArt);
  bool togglePictureInPicture(PlaybackOutputController& output,
                              bool enableAscii, bool audioOnlyPlayback,
                              bool& redraw, bool& forceRefreshArt);
  bool toggleFullscreen(PlaybackOutputController& output, bool enableAscii,
                        bool& redraw, bool& forceRefreshArt);

  void closePresentation(PlaybackOutputController& output, bool& redraw,
                         bool& forceRefreshArt);
  void reconcile(PlaybackOutputController& output);
  void handleWindowClosed(PlaybackOutputController& output, bool& redraw,
                          bool& forceRefreshArt);
  void captureWindowPlacement(PlaybackOutputController& output,
                              PlaybackSessionContinuationState& state) const;

 private:
  struct PendingWindowPresentation {
    bool active = false;
    PlaybackPresentationMode target = PlaybackPresentationMode::Fullscreen;
    bool textGrid = false;
  };

  void clearPendingWindowPresentation();
  void requestWindowPresentation(PlaybackPresentationMode target,
                                 bool textGrid);

  PendingWindowPresentation pendingWindowPresentation;
  bool pictureInPictureStartedFromTerminal = false;
};
