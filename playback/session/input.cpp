#include "input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "audioplayback.h"
#include "playback/overlay/overlay.h"
#include "playback/video/player.h"
#include "playback/input/shortcuts.h"
#include "playback/session/osd_timeline.h"
#include "handoff.h"
#include "playback/video/subtitle/manager.h"
#include "ui_inputlogic.h"
#include "playback/video/framebuffer/window/window.h"

namespace playback_session_input {

void setPlaybackPaused(const PlaybackInputView& view,
                       PlaybackInputSignals& signals,
                       PlaybackSeekGestureState& seekState, bool paused);

namespace {

bool hasOverlayVisibleWindow(const PlaybackInputSignals& signals) {
  return signals.osd->controlsVisible();
}

void requestWindowRefresh(const PlaybackInputSignals& signals) {
  signals.requestWindowPresent();
}

void updateOverlayControlHover(PlaybackInputSignals& signals, int nextHover) {
  const int previousHover = signals.overlayControlHover->exchange(
      nextHover, std::memory_order_relaxed);
  if (nextHover == previousHover) {
    return;
  }
  *signals.redraw = true;
  requestWindowRefresh(signals);
}

double playbackDurationSec(const PlaybackInputView& view) {
  const int64_t durationUs = view.player->durationUs();
  if (durationUs > 0) {
    return static_cast<double>(durationUs) / 1000000.0;
  }
  return audioGetTotalSec();
}

double clampPlaybackSeekTarget(const PlaybackInputView& view,
                               double targetSec) {
  double target = std::isfinite(targetSec) ? targetSec : 0.0;
  target = std::max(0.0, target);
  const double totalSec = playbackDurationSec(view);
  if (totalSec > 0.0 && std::isfinite(totalSec)) {
    target = std::min(target, totalSec);
  }
  return target;
}

bool readQueuedSeekTargetSec(const PlaybackSeekGestureState& seekState,
                             double* outTargetSec) {
  if (!seekState.seekQueued) {
    return false;
  }
  const double targetSec = seekState.queuedSeekTargetSec;
  if (!(targetSec >= 0.0) || !std::isfinite(targetSec)) {
    return false;
  }
  if (outTargetSec) {
    *outTargetSec = targetSec;
  }
  return true;
}

bool pauseRequestedByToggle(const PlaybackInputView& view) {
  return playback_session_state::toggleRequestsPause(*view.playbackState,
                                                      view.player->isEnded());
}

bool queuePlaybackSeekToRatio(const PlaybackInputView& view,
                              PlaybackInputSignals& signals,
                              PlaybackSeekGestureState& seekState,
                              double ratio) {
  const double totalSec = playbackDurationSec(view);
  if (!(totalSec > 0.0) || !std::isfinite(totalSec)) {
    return false;
  }
  queueSeekRequest(signals, seekState, std::clamp(ratio, 0.0, 1.0) * totalSec);
  return true;
}

std::optional<int64_t> playbackTimelineTargetForRatio(
    const PlaybackInputView& view, double ratio) {
  const int64_t durationUs = view.player->durationUs();
  if (durationUs <= 0 || !std::isfinite(ratio)) return std::nullopt;
  return static_cast<int64_t>(std::llround(
      std::clamp(ratio, 0.0, 1.0) * static_cast<double>(durationUs)));
}

void triggerOverlay(const PlaybackInputView& view,
                    const PlaybackInputSignals& signals) {
  bool extended = false;
  if (*view.playbackState == PlaybackSessionState::Paused) {
    extended = true;
  }
  if (*view.playbackState == PlaybackSessionState::Ended) {
    extended = true;
  }
  if (view.player->seekPending()) {
    extended = true;
  }
  constexpr auto kProgressOverlayTimeout = std::chrono::milliseconds(1750);
  constexpr auto kProgressOverlayExtendedTimeout =
      std::chrono::milliseconds(2500);
  const auto timeout = extended ? kProgressOverlayExtendedTimeout
                                : kProgressOverlayTimeout;
  signals.osd->showControls(playback_session::PlaybackOsdTimeline::Clock::now(),
                            timeout);
  requestWindowRefresh(signals);
}

void requestPlaybackExit(const PlaybackInputView& view,
                         PlaybackInputSignals& signals, bool quitApp) {
  if (signals.requestPlaybackExit &&
      !signals.requestPlaybackExit(quitApp)) {
    return;
  }
  *view.playbackState = PlaybackSessionState::Exiting;
  *signals.loopStopRequested = true;
  signals.osd->clear();
  *signals.redraw = true;
  *signals.forceRefreshArt = true;
  if (quitApp && signals.quitApplicationRequested) {
    *signals.quitApplicationRequested = true;
  }
}

void refreshPlaybackInputDisplay(PlaybackInputSignals& signals) {
  *signals.forceRefreshArt = true;
  *signals.redraw = true;
}

void clearQueuedSeek(PlaybackSeekGestureState& seekState) {
  seekState.queuedSeekTargetSec = -1.0;
  seekState.seekQueued = false;
}

void markSeekSent(PlaybackInputSignals& signals,
                  PlaybackSeekGestureState& seekState) {
  seekState.lastSeekSentTime = std::chrono::steady_clock::now();
  clearQueuedSeek(seekState);
  refreshPlaybackInputDisplay(signals);
}

void refreshFrameStepRequestDisplay(PlaybackInputSignals& signals) {
  refreshPlaybackInputDisplay(signals);
  requestWindowRefresh(signals);
}

void commitQueuedSeek(const PlaybackInputView& view,
                      PlaybackInputSignals& signals,
                      PlaybackSeekGestureState& seekState) {
  double queuedTargetSec = 0.0;
  if (readQueuedSeekTargetSec(seekState, &queuedTargetSec)) {
    sendSeekRequest(view, signals, seekState, queuedTargetSec);
  }
}

void finishVideoEditBoundaryDrag(
    const PlaybackInputView& view, PlaybackInputSignals& signals,
    PlaybackSeekGestureState& seekState,
    playback_video_timeline_preview::PresentationSurface surface) {
  if (!seekState.videoEditBoundaryDrag ||
      seekState.videoEditBoundaryDrag->surface != surface) {
    return;
  }
  const PlaybackSeekGestureState::VideoEditBoundaryDrag completed =
      *seekState.videoEditBoundaryDrag;
  commitQueuedSeek(view, signals, seekState);
  const PlayerTimelineSnapshot timeline = view.player->timelineSnapshot();
  if (completed.targetTimelineUs >= 0 &&
      timeline.latestSeekRequestGeneration >
          completed.seekGenerationAtStart) {
    seekState.pendingVideoEditBoundaryCommit =
        PlaybackSeekGestureState::PendingVideoEditBoundaryCommit{
            completed.boundary, timeline.latestSeekRequestGeneration};
  }
  seekState.videoEditBoundaryDrag.reset();
}

void sendRelativeSeekRequest(const PlaybackInputView& view,
                             PlaybackInputSignals& signals,
                             PlaybackSeekGestureState& seekState,
                             int64_t deltaUs) {
  commitQueuedSeek(view, signals, seekState);
  if (!view.player->requestRelativeSeek(deltaUs)) {
    return;
  }
  markSeekSent(signals, seekState);
  if (view.timingSink) {
    const PlayerTimelineSnapshot timeline = view.player->timelineSnapshot();
    char buf[256];
    std::snprintf(
        buf, sizeof(buf),
        "seek_relative_request delta_us=%lld target_us=%lld generation=%llu",
        static_cast<long long>(deltaUs),
        static_cast<long long>(timeline.positionUs),
        static_cast<unsigned long long>(
            timeline.latestSeekRequestGeneration));
    view.timingSink(std::string(buf));
  }
}

bool toggleRequestedLayout(const PlaybackInputView& view,
                           PlaybackInputSignals& signals) {
  (void)view;
  return signals.toggleWindowPresentation();
}

bool toggleSubtitles(const PlaybackInputView& view) {
  if (!view.hasSubtitles) {
    return false;
  }
  const bool enabled =
      view.enableSubtitlesShared->load(std::memory_order_relaxed);
  if (!enabled) {
    view.subtitleManager->selectFirstTrackWithCues();
    view.enableSubtitlesShared->store(true, std::memory_order_relaxed);
    return true;
  }
  const size_t count = view.subtitleManager->selectableTrackCount();
  if (count <= 1) {
    view.enableSubtitlesShared->store(false, std::memory_order_relaxed);
    return true;
  }
  if (view.subtitleManager->isActiveLastCueTrack()) {
    view.enableSubtitlesShared->store(false, std::memory_order_relaxed);
    return true;
  }
  return view.subtitleManager->cycleLanguage();
}

bool toggleAudioTrack(const PlaybackInputView& view) {
  if (!*view.audioOk) {
    return false;
  }
  return view.player->cycleAudioTrack();
}

bool cycleRadioFilter(const PlaybackInputView& view) {
  if (!*view.audioOk) {
    return false;
  }
  audioCycleRadioFilter();
  return true;
}

bool toggle50Hz(const PlaybackInputView& view) {
  if (!*view.audioOk || !audioSupports50HzToggle()) {
    return false;
  }
  audioToggle50Hz();
  return true;
}

bool togglePictureInPicture(const PlaybackInputView& view,
                            const PlaybackInputSignals& signals) {
  (void)view;
  return signals.togglePictureInPicture();
}

bool requestFrameStep(const PlaybackInputView& view,
                      PlaybackInputSignals& signals,
                      PlaybackSeekGestureState& seekState,
                      playback_video_frame_step::Direction direction) {
  commitQueuedSeek(view, signals, seekState);

  if (!view.player->requestFrameStep(direction)) {
    return false;
  }
  *view.playbackState = PlaybackSessionState::Paused;
  refreshFrameStepRequestDisplay(signals);
  return true;
}

bool executeOverlayControl(const PlaybackInputView& view,
                           PlaybackInputSignals& signals,
                           PlaybackSeekGestureState& seekState,
                           playback_overlay::OverlayControlId control) {
  playback_overlay::OverlayControlActions actions;
  actions.previous = [&]() {
    return playback_session_handoff::requestTransportHandoff(
        view, signals, PlaybackTransportCommand::Previous);
  };
  actions.playPause = [&]() {
    setPlaybackPaused(view, signals, seekState, pauseRequestedByToggle(view));
    return true;
  };
  actions.next = [&]() {
    return playback_session_handoff::requestTransportHandoff(
        view, signals, PlaybackTransportCommand::Next);
  };
  actions.radio = [&]() { return cycleRadioFilter(view); };
  actions.hz50 = [&]() { return toggle50Hz(view); };
  actions.audioTrack = [&]() { return toggleAudioTrack(view); };
  actions.subtitles = [&]() { return toggleSubtitles(view); };
  actions.pictureInPicture = [&]() {
    return togglePictureInPicture(view, signals);
  };
  actions.videoEdit = [&](playback_video_edit::Command command) {
    return signals.executeVideoEditCommand &&
           signals.executeVideoEditCommand(command);
  };
  actions.waitForVideoEditExport = [&]() {
    return signals.waitForVideoEditExportAndExit &&
           signals.waitForVideoEditExportAndExit();
  };
  actions.confirmPendingExit = [&]() {
    return signals.confirmPendingExit && signals.confirmPendingExit();
  };
  actions.cancelPendingExit = [&]() {
    return signals.cancelPendingExit && signals.cancelPendingExit();
  };
  return playback_overlay::dispatchOverlayControl(control, actions);
}

bool dispatchContextMenuInput(
    PlaybackInputSignals& signals,
    const playback_session::ContextMenuInput& request) {
  if (!signals.handleContextMenuInput) return false;
  if (!signals.handleContextMenuInput(request)) return false;
  updateOverlayControlHover(signals, -1);
  *signals.redraw = true;
  requestWindowRefresh(signals);
  return true;
}

}  // namespace

bool isOverlayVisible(const PlaybackInputSignals& signals) {
  return hasOverlayVisibleWindow(signals);
}

void sendSeekRequest(const PlaybackInputView& view,
                     PlaybackInputSignals& signals,
                     PlaybackSeekGestureState& seekState, double targetSec) {
  targetSec = clampPlaybackSeekTarget(view, targetSec);
  int64_t targetUs =
      static_cast<int64_t>(std::llround(targetSec * 1000000.0));
  if (!view.player->requestSeek(targetUs)) {
    return;
  }
  markSeekSent(signals, seekState);
  if (view.timingSink) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "seek_request target_sec=%.3f target_us=%lld",
                  targetSec, static_cast<long long>(targetUs));
    view.timingSink(std::string(buf));
  }
}

void queueSeekRequest(PlaybackInputSignals& signals,
                      PlaybackSeekGestureState& seekState, double targetSec) {
  seekState.queuedSeekTargetSec = targetSec;
  seekState.seekQueued = true;
  refreshPlaybackInputDisplay(signals);
}

void setPlaybackPaused(const PlaybackInputView& view,
                       PlaybackInputSignals& signals,
                       PlaybackSeekGestureState& seekState, bool paused) {
  commitQueuedSeek(view, signals, seekState);
  const bool ended = *view.playbackState == PlaybackSessionState::Ended ||
                     view.player->isEnded();
  if (!paused && ended) {
    const PlayerTimelineSnapshot timeline = view.player->timelineSnapshot();
    const int64_t durationUs = view.player->durationUs();
    const bool pendingAwayFromEnd =
        timeline.seekPending() &&
        (durationUs <= 0 || timeline.positionUs < durationUs);
    if (!pendingAwayFromEnd) {
      view.player->requestSeek(0);
    }
    view.player->setVideoPaused(false);
    *view.playbackState = PlaybackSessionState::Active;
    return;
  }

  if (paused && ended) {
    view.player->setVideoPaused(true);
    return;
  }

  bool pausedNow = paused;
  view.player->setVideoPaused(pausedNow);
  *view.playbackState =
      pausedNow ? PlaybackSessionState::Paused : PlaybackSessionState::Active;
}

void handlePlaybackInputEvent(const PlaybackInputView& view,
                              PlaybackInputSignals& signals,
                              PlaybackSeekGestureState& seekState,
                              const InputEvent& ev) {
  if (signals.contextMenuVisible && signals.contextMenuVisible()) {
    playback_session::ContextMenuInput request;
    if (ev.type == InputEvent::Type::Action &&
        ev.action == InputAction::Back) {
      request.kind = playback_session::ContextMenuInputKind::Dismiss;
      dispatchContextMenuInput(signals, request);
      return;
    }
    if (ev.type == InputEvent::Type::Key) {
      if (ev.key.vk == VK_ESCAPE || ev.key.vk == VK_BACK) {
        request.kind = playback_session::ContextMenuInputKind::Dismiss;
      } else if (ev.key.vk == VK_UP) {
        request.kind = playback_session::ContextMenuInputKind::MoveSelection;
        request.selectionDelta = -1;
      } else if (ev.key.vk == VK_DOWN) {
        request.kind = playback_session::ContextMenuInputKind::MoveSelection;
        request.selectionDelta = 1;
      } else if (ev.key.vk == VK_RETURN) {
        request.kind = playback_session::ContextMenuInputKind::ActivateSelection;
      } else {
        return;
      }
      dispatchContextMenuInput(signals, request);
      return;
    }
  }
  InputCallbacks cb;
  cb.onQuit = [&]() { requestPlaybackExit(view, signals, true); };
  cb.onPlay = [&]() {
    setPlaybackPaused(view, signals, seekState, false);
  };
  cb.onPause = [&]() {
    setPlaybackPaused(view, signals, seekState, true);
  };
  cb.onTogglePause = [&]() {
    setPlaybackPaused(view, signals, seekState, pauseRequestedByToggle(view));
  };
  cb.onStopPlayback = [&]() { requestPlaybackExit(view, signals, false); };
  cb.onPlayPrevious = [&]() {
    playback_session_handoff::requestTransportHandoff(
        view, signals, PlaybackTransportCommand::Previous);
  };
  cb.onPlayNext = [&]() {
    playback_session_handoff::requestTransportHandoff(
        view, signals, PlaybackTransportCommand::Next);
  };
  cb.onToggleWindow = [&]() { toggleRequestedLayout(view, signals); };
  cb.onToggleFullscreen = [&]() { signals.toggleFullscreen(); };
  cb.onToggleRadio = [&]() { cycleRadioFilter(view); };
  cb.onToggle50Hz = [&]() { toggle50Hz(view); };
  cb.onToggleSubtitles = [&]() { toggleSubtitles(view); };
  cb.onToggleAudioTrack = [&]() { toggleAudioTrack(view); };
  cb.onPlaybackContextShortcut = [&](PlaybackShortcutAction action) {
    if (const auto command = videoEditCommandForShortcut(action)) {
      if (signals.executeVideoEditCommand) {
        signals.executeVideoEditCommand(*command);
      }
      return;
    }
    switch (action) {
      case PlaybackShortcutAction::TogglePictureInPicture:
      case PlaybackShortcutAction::DismissPictureInPicture:
        togglePictureInPicture(view, signals);
        break;
      case PlaybackShortcutAction::NavigateBackInVideoEditor:
      case PlaybackShortcutAction::ExitPlaybackSession:
        if (signals.navigateBack) signals.navigateBack();
        break;
      case PlaybackShortcutAction::DiscardVideoEditsAndExit:
        if (signals.confirmPendingExit) {
          signals.confirmPendingExit();
        }
        break;
      case PlaybackShortcutAction::CancelVideoEditPrompt:
        if (signals.navigateBack) signals.navigateBack();
        break;
      default:
        break;
    }
  };
  cb.onSeekBy = [&](int dir) {
    sendRelativeSeekRequest(view, signals, seekState,
                            static_cast<int64_t>(dir) * 5000000);
  };
  cb.onPreviousFrame = [&]() {
    requestFrameStep(view, signals, seekState,
                     playback_video_frame_step::Direction::Previous);
  };
  cb.onNextFrame = [&]() {
    requestFrameStep(view, signals, seekState,
                     playback_video_frame_step::Direction::Next);
  };
  cb.onCopyVideoFrame = signals.copyCurrentVideoFrameToClipboard;
  cb.onAdjustVolume = [&](float delta) { audioAdjustVolume(delta); };

  const playback_video_edit::Prompt editPrompt =
      signals.videoEditPrompt ? signals.videoEditPrompt()
                              : playback_video_edit::Prompt::None;
  uint32_t shortcutContexts = 0;
  if (editPrompt == playback_video_edit::Prompt::LeaveEditMode) {
    shortcutContexts = kPlaybackShortcutContextVideoEditLeaveConfirmation;
  } else if (editPrompt == playback_video_edit::Prompt::DiscardEdits) {
    shortcutContexts = kPlaybackShortcutContextVideoEditDiscardConfirmation;
  } else if (editPrompt == playback_video_edit::Prompt::LeavePlayback) {
    shortcutContexts = kPlaybackShortcutContextVideoEditExitConfirmation;
  } else {
    shortcutContexts = kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextPlaybackSession |
                       kPlaybackShortcutContextVideoPlayback;
    if (signals.videoEditorActive && signals.videoEditorActive()) {
      shortcutContexts |= kPlaybackShortcutContextVideoEditing;
    }
    if (view.videoWindow && view.videoWindow->IsPictureInPicture()) {
      shortcutContexts |= kPlaybackShortcutContextPictureInPicture;
    }
  }
  const PlaybackInputResult playbackResult =
      handlePlaybackInput(ev, cb, shortcutContexts);
  if (playbackResult == PlaybackInputResult::Handled) {
    if (*signals.loopStopRequested) {
      return;
    }
    triggerOverlay(view, signals);
    *signals.redraw = true;
    return;
  }
  if (playbackResult == PlaybackInputResult::HandledWithoutOverlayRefresh) {
    return;
  }
}

void handlePlaybackControlCommand(const PlaybackInputView& view,
                                  PlaybackInputSignals& signals,
                                  PlaybackSeekGestureState& seekState,
                                  PlaybackControlCommand command) {
  if (signals.videoEditPrompt &&
      signals.videoEditPrompt() != playback_video_edit::Prompt::None) {
    return;
  }
  switch (command) {
    case PlaybackControlCommand::Play:
      setPlaybackPaused(view, signals, seekState, false);
      break;
    case PlaybackControlCommand::Pause:
      setPlaybackPaused(view, signals, seekState, true);
      break;
    case PlaybackControlCommand::TogglePause: {
      setPlaybackPaused(view, signals, seekState, pauseRequestedByToggle(view));
      break;
    }
    case PlaybackControlCommand::Stop:
      requestPlaybackExit(view, signals, false);
      break;
    case PlaybackControlCommand::Previous:
      playback_session_handoff::requestTransportHandoff(
          view, signals, PlaybackTransportCommand::Previous);
      break;
    case PlaybackControlCommand::Next:
      playback_session_handoff::requestTransportHandoff(
          view, signals, PlaybackTransportCommand::Next);
      break;
  }
  if (*signals.loopStopRequested) {
    return;
  }
  triggerOverlay(view, signals);
  *signals.redraw = true;
}

void handlePlaybackMouseEvent(const PlaybackInputView& view,
                              PlaybackInputSignals& signals,
                              PlaybackSeekGestureState& seekState,
                              const MouseEvent& mouse) {
  const bool windowEvent = isWindowMouseEvent(mouse);
  const auto previewSurface =
      windowEvent
          ? playback_video_timeline_preview::PresentationSurface::VideoWindow
          : playback_video_timeline_preview::PresentationSurface::Terminal;
  const bool leftPressed =
      (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
  const bool rightPressed =
      (mouse.buttonState & RIGHTMOST_BUTTON_PRESSED) != 0;
  const bool dragFromThisSurface =
      seekState.videoEditBoundaryDrag &&
      seekState.videoEditBoundaryDrag->surface == previewSurface;
  const bool progressDragFromThisSurface =
      seekState.progressDragSurface == previewSurface;
  if ((dragFromThisSurface || progressDragFromThisSurface) && !leftPressed) {
    finishVideoEditBoundaryDrag(view, signals, seekState, previewSurface);
    seekState.progressDragSurface.reset();
    commitQueuedSeek(view, signals, seekState);
    *signals.redraw = true;
  }
  const playback_video_edit::Prompt editPrompt =
      signals.videoEditPrompt ? signals.videoEditPrompt()
                              : playback_video_edit::Prompt::None;

  const double pointerX =
      windowEvent && mouse.hasPixelPosition ? mouse.pixelX : mouse.pos.X;
  const double pointerY =
      windowEvent && mouse.hasPixelPosition ? mouse.pixelY : mouse.pos.Y;
  if (isBackMousePressed(mouse)) {
    if (signals.contextMenuVisible && signals.contextMenuVisible()) {
      playback_session::ContextMenuInput request;
      request.kind = playback_session::ContextMenuInputKind::Dismiss;
      dispatchContextMenuInput(signals, request);
    } else if (signals.navigateBack) {
      signals.navigateBack();
    }
    return;
  }
  if (mouse.eventFlags == MOUSE_MOVED) {
    triggerOverlay(view, signals);
    *signals.redraw = true;
  }

  const bool capturedProgressDrag =
      (dragFromThisSurface || progressDragFromThisSurface) && leftPressed;
  playback_overlay::InteractionHit interactionHit;
  if (windowEvent) {
    interactionHit = view.videoWindow->OverlayHitAt(
        pointerX, pointerY, capturedProgressDrag);
  } else if (view.frameOutputState) {
    const playback_overlay::InteractionMap& interactions =
        view.frameOutputState->overlayInteractions;
    if (mouse.hasPixelPosition) {
      interactionHit = playback_overlay::interactionHitAtTransformed(
          interactions, 0.0, 0.0, std::max(1.0, mouse.unitWidth),
          std::max(1.0, mouse.unitHeight), mouse.pixelX, mouse.pixelY,
          capturedProgressDrag);
    } else {
      interactionHit = playback_overlay::interactionHitAt(
          interactions, pointerX, pointerY, capturedProgressDrag);
    }
  }
  const auto& progressHit = interactionHit.progressBar;
  const auto& boundaryHit = interactionHit.editBoundary;
  const auto& controlHit = interactionHit.control;
  const auto& contextMenuItemHit = interactionHit.contextMenuItem;
  if (rightPressed && mouse.eventFlags == 0 &&
      editPrompt == playback_video_edit::Prompt::None) {
    playback_session::ContextMenuInput request;
    request.kind = playback_session::ContextMenuInputKind::Open;
    request.surface =
        windowEvent ? playback_session::ContextMenuSurface::VideoWindow
                    : playback_session::ContextMenuSurface::Terminal;
    request.x = pointerX;
    request.y = pointerY;
    if (progressHit) {
      request.timelineUs =
          playbackTimelineTargetForRatio(view, progressHit->ratio);
      const int64_t durationUs = view.player->durationUs();
      if (durationUs > 0) {
        request.timelineToleranceUs = std::max<int64_t>(
            1, durationUs / std::max(1, progressHit->units - 1));
      }
    }
    if (dispatchContextMenuInput(signals, request)) return;
  }
  if (signals.contextMenuVisible && signals.contextMenuVisible()) {
    playback_session::ContextMenuInput request;
    request.surface =
        windowEvent ? playback_session::ContextMenuSurface::VideoWindow
                    : playback_session::ContextMenuSurface::Terminal;
    if (mouse.eventFlags == MOUSE_WHEELED) {
      const int delta = static_cast<SHORT>(HIWORD(mouse.buttonState));
      if (delta != 0) {
        request.kind = playback_session::ContextMenuInputKind::MoveSelection;
        request.selectionDelta = delta > 0 ? -1 : 1;
        dispatchContextMenuInput(signals, request);
      }
      return;
    }
    if (mouse.eventFlags == MOUSE_MOVED) {
      if (contextMenuItemHit) {
        request.kind = playback_session::ContextMenuInputKind::SelectItem;
        request.item = *contextMenuItemHit;
        dispatchContextMenuInput(signals, request);
      }
      return;
    }
    if (leftPressed && mouse.eventFlags == 0) {
      if (contextMenuItemHit) {
        request.kind = playback_session::ContextMenuInputKind::ActivateItem;
        request.item = *contextMenuItemHit;
      } else {
        request.kind = playback_session::ContextMenuInputKind::Dismiss;
      }
      dispatchContextMenuInput(signals, request);
      return;
    }
    return;
  }
  if (progressHit) {
    triggerOverlay(view, signals);
    *signals.redraw = true;
  }

  const double progressRatio = progressHit ? progressHit->ratio : 0.0;
  const int progressUnits = progressHit ? progressHit->units : 0;

  if (progressHit && leftPressed && !dragFromThisSurface &&
      mouse.eventFlags == 0 && boundaryHit &&
      signals.videoEditorActive && signals.videoEditorActive() &&
      signals.moveVideoEditBoundary) {
    setPlaybackPaused(view, signals, seekState, true);
    seekState.pendingVideoEditBoundaryCommit.reset();
    seekState.videoEditBoundaryDrag =
        PlaybackSeekGestureState::VideoEditBoundaryDrag{
            *boundaryHit, previewSurface, -1,
            view.player->timelineSnapshot().latestSeekRequestGeneration};
  } else if (progressHit && leftPressed && mouse.eventFlags == 0) {
    seekState.progressDragSurface = previewSurface;
  }
  const bool boundaryDrag =
      seekState.videoEditBoundaryDrag &&
      seekState.videoEditBoundaryDrag->surface == previewSurface;
  if (progressHit && leftPressed && boundaryDrag) {
    double previewRatio = progressRatio;
    if (const auto targetUs =
            playbackTimelineTargetForRatio(view, progressRatio)) {
      seekState.videoEditBoundaryDrag->targetTimelineUs = *targetUs;
      signals.moveVideoEditBoundary(seekState.videoEditBoundaryDrag->boundary,
                                    *targetUs);
      const int64_t seekTargetUs = videoEditBoundarySeekTargetUs(
          seekState.videoEditBoundaryDrag->boundary, *targetUs);
      queueSeekRequest(signals, seekState,
                       static_cast<double>(seekTargetUs) / 1000000.0);
      const int64_t durationUs = view.player->durationUs();
      if (durationUs > 0) {
        previewRatio = static_cast<double>(seekTargetUs) /
                       static_cast<double>(durationUs);
      }
    }
    updateOverlayControlHover(signals, -1);
    if (signals.requestTimelinePreview) {
      signals.requestTimelinePreview(previewSurface, previewRatio,
                                     progressUnits);
    }
    return;
  }
  const bool seekGesture =
      leftPressed && progressHit &&
      (windowEvent || mouse.eventFlags == 0 ||
       mouse.eventFlags == MOUSE_MOVED);
  if (progressHit) {
    updateOverlayControlHover(signals, -1);
    if (signals.requestTimelinePreview) {
      signals.requestTimelinePreview(previewSurface, progressRatio,
                                     progressUnits);
    }
    if (seekGesture) {
      queuePlaybackSeekToRatio(view, signals, seekState, progressRatio);
    }
    return;
  }
  if (signals.clearTimelinePreview) {
    signals.clearTimelinePreview(previewSurface);
  }
  updateOverlayControlHover(
      signals, controlHit ? playback_overlay::overlayControlToken(*controlHit)
                          : -1);
  if (controlHit) {
    triggerOverlay(view, signals);
  }

  if (leftPressed && mouse.eventFlags == 0 && controlHit) {
    if (executeOverlayControl(view, signals, seekState, *controlHit)) {
      updateOverlayControlHover(signals, -1);
      if (*signals.loopStopRequested) {
        return;
      }
      triggerOverlay(view, signals);
      *signals.redraw = true;
    }
    return;
  }

  if (leftPressed && windowEvent) {
    return;
  }
}

void handlePlaybackPointerLeave(PlaybackInputSignals& signals,
                                PlaybackSeekGestureState& seekState,
                                const PlaybackInputView& view) {
  finishVideoEditBoundaryDrag(
      view, signals, seekState,
      playback_video_timeline_preview::PresentationSurface::VideoWindow);
  seekState.progressDragSurface.reset();
  commitQueuedSeek(view, signals, seekState);
  if (signals.clearTimelinePreview) {
    signals.clearTimelinePreview(
        playback_video_timeline_preview::PresentationSurface::VideoWindow);
  }
  updateOverlayControlHover(signals, -1);
}

}  // namespace playback_session_input
