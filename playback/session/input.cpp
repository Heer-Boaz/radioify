#include "input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "audioplayback.h"
#include "playback/overlay/overlay.h"
#include "playback/input/shortcuts.h"
#include "playback/session/osd_timeline.h"
#include "playback/video/subtitle/manager.h"
#include "ui_inputlogic.h"
#include "playback/video/framebuffer/window/window.h"

namespace playback_session_input {

void setPlaybackPaused(const PlaybackInputView& view,
                       PlaybackInputSignals& signals,
                       PlaybackSeekGestureState& seekState, bool paused);

namespace {

bool requestTransport(PlaybackInputSignals& signals,
                      PlaybackTransportCommand command) {
  return signals.commands.dispatch(TransportRequest{command});
}

bool hasOverlayVisibleWindow(const PlaybackInputSignals& signals) {
  return signals.osd.controlsVisible();
}

void requestWindowRefresh(const PlaybackInputSignals& signals) {
  signals.commands.dispatch(CommandAction::RequestWindowPresent);
}

void updateOverlayControlHover(PlaybackInputSignals& signals, int nextHover) {
  const int previousHover = signals.overlayControlHover.exchange(
      nextHover, std::memory_order_relaxed);
  if (nextHover == previousHover) {
    return;
  }
  signals.redraw = true;
  requestWindowRefresh(signals);
}

double playbackDurationSec(const PlaybackInputView& view) {
  const int64_t durationUs = view.transport.snapshot().durationUs;
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
  const TransportSnapshot transport = view.transport.snapshot();
  return playback_session_state::toggleRequestsPause(
      transport.state, transport.ended);
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
  const int64_t durationUs = view.transport.snapshot().durationUs;
  if (durationUs <= 0 || !std::isfinite(ratio)) return std::nullopt;
  return static_cast<int64_t>(std::llround(
      std::clamp(ratio, 0.0, 1.0) * static_cast<double>(durationUs)));
}

void triggerOverlay(const PlaybackInputView& view,
                    const PlaybackInputSignals& signals) {
  const TransportSnapshot transport = view.transport.snapshot();
  const bool extended = transport.state == PlaybackSessionState::Paused ||
                        transport.state == PlaybackSessionState::Ended ||
                        transport.seekPending;
  constexpr auto kProgressOverlayTimeout = std::chrono::milliseconds(1750);
  constexpr auto kProgressOverlayExtendedTimeout =
      std::chrono::milliseconds(2500);
  const auto timeout = extended ? kProgressOverlayExtendedTimeout
                                : kProgressOverlayTimeout;
  signals.osd.showControls(playback_session::PlaybackOsdTimeline::Clock::now(),
                           timeout);
  requestWindowRefresh(signals);
}

void requestPlaybackExit(PlaybackInputSignals& signals, bool quitApp) {
  signals.commands.dispatch(PlaybackExitRequest{quitApp});
}

void refreshPlaybackInputDisplay(PlaybackInputSignals& signals) {
  signals.forceRefreshArt = true;
  signals.redraw = true;
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
  const TransportSnapshot transport = view.transport.snapshot();
  if (completed.targetTimelineUs >= 0 &&
      transport.latestSeekRequestGeneration >
          completed.seekGenerationAtStart) {
    seekState.pendingVideoEditBoundaryCommit =
        PlaybackSeekGestureState::PendingVideoEditBoundaryCommit{
            completed.boundary, transport.latestSeekRequestGeneration};
  }
  seekState.videoEditBoundaryDrag.reset();
}

void sendRelativeSeekRequest(const PlaybackInputView& view,
                             PlaybackInputSignals& signals,
                             PlaybackSeekGestureState& seekState,
                             int64_t deltaUs) {
  commitQueuedSeek(view, signals, seekState);
  if (!view.transport.seekBy(deltaUs)) {
    return;
  }
  markSeekSent(signals, seekState);
  if (view.timingSink) {
    const TransportSnapshot transport = view.transport.snapshot();
    char buf[256];
    std::snprintf(
        buf, sizeof(buf),
        "seek_relative_request delta_us=%lld target_us=%lld generation=%llu",
        static_cast<long long>(deltaUs),
        static_cast<long long>(transport.positionUs),
        static_cast<unsigned long long>(
            transport.latestSeekRequestGeneration));
    view.timingSink(std::string(buf));
  }
}

bool toggleRequestedLayout(const PlaybackInputView& view,
                           PlaybackInputSignals& signals) {
  (void)view;
  return signals.commands.dispatch(CommandAction::ToggleWindowPresentation);
}

bool toggleSubtitles(const PlaybackInputView& view) {
  if (!view.hasSubtitles) {
    return false;
  }
  std::lock_guard<std::mutex> subtitleLock(view.subtitleMutex);
  const bool enabled =
      view.enableSubtitlesShared.load(std::memory_order_relaxed);
  if (!enabled) {
    view.subtitleManager.selectFirstTrackWithCues();
    view.enableSubtitlesShared.store(true, std::memory_order_relaxed);
    return true;
  }
  const size_t count = view.subtitleManager.selectableTrackCount();
  if (count <= 1) {
    view.enableSubtitlesShared.store(false, std::memory_order_relaxed);
    return true;
  }
  if (view.subtitleManager.isActiveLastCueTrack()) {
    view.enableSubtitlesShared.store(false, std::memory_order_relaxed);
    return true;
  }
  return view.subtitleManager.cycleLanguage();
}

bool toggleAudioTrack(const PlaybackInputView& view) {
  return view.transport.cycleAudioTrack();
}

bool cycleRadioFilter(const PlaybackInputView& view) {
  if (!view.transport.snapshot().audioAvailable) {
    return false;
  }
  audioCycleRadioFilter();
  return true;
}

bool toggle50Hz(const PlaybackInputView& view) {
  if (!view.transport.snapshot().audioAvailable ||
      !audioSupports50HzToggle()) {
    return false;
  }
  audioToggle50Hz();
  return true;
}

bool togglePictureInPicture(const PlaybackInputView& view,
                            const PlaybackInputSignals& signals) {
  (void)view;
  return signals.commands.dispatch(CommandAction::TogglePictureInPicture);
}

bool requestFrameStep(const PlaybackInputView& view,
                      PlaybackInputSignals& signals,
                      PlaybackSeekGestureState& seekState,
                      playback_video_frame_step::Direction direction) {
  commitQueuedSeek(view, signals, seekState);

  if (!view.transport.requestFrameStep(direction)) {
    return false;
  }
  refreshFrameStepRequestDisplay(signals);
  return true;
}

bool executeOverlayControl(const PlaybackInputView& view,
                           PlaybackInputSignals& signals,
                           PlaybackSeekGestureState& seekState,
                           playback_overlay::OverlayControlId control) {
  playback_overlay::OverlayControlActions actions;
  actions.previous = [&]() {
    return requestTransport(signals, PlaybackTransportCommand::Previous);
  };
  actions.playPause = [&]() {
    setPlaybackPaused(view, signals, seekState, pauseRequestedByToggle(view));
    return true;
  };
  actions.next = [&]() {
    return requestTransport(signals, PlaybackTransportCommand::Next);
  };
  actions.radio = [&]() { return cycleRadioFilter(view); };
  actions.hz50 = [&]() { return toggle50Hz(view); };
  actions.audioTrack = [&]() { return toggleAudioTrack(view); };
  actions.subtitles = [&]() { return toggleSubtitles(view); };
  actions.pictureInPicture = [&]() {
    return togglePictureInPicture(view, signals);
  };
  actions.videoEdit = [&](playback_video_edit::Command command) {
    return signals.commands.dispatch(VideoEditRequest{command});
  };
  actions.waitForVideoEditExport = [&]() {
    return signals.commands.dispatch(
        CommandAction::WaitForVideoEditExportAndExit);
  };
  actions.confirmPendingExit = [&]() {
    return signals.commands.dispatch(CommandAction::ConfirmPendingExit);
  };
  actions.cancelPendingExit = [&]() {
    return signals.commands.dispatch(CommandAction::CancelPendingExit);
  };
  return playback_overlay::dispatchOverlayControl(control, actions);
}

bool dispatchContextMenuInput(
    PlaybackInputSignals& signals,
    const playback_session::ContextMenuInput& request) {
  if (!signals.commands.dispatch(ContextMenuRequest{request})) return false;
  updateOverlayControlHover(signals, -1);
  signals.redraw = true;
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
  if (!view.transport.seekTo(targetUs)) {
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
  view.transport.setPaused(paused);
}

namespace {

void dispatchPlaybackInputCommand(
    const PlaybackInputView& view, PlaybackInputSignals& signals,
    PlaybackSeekGestureState& seekState, playback_input::Command command) {
  const auto* action = std::get_if<PlaybackAction>(&command);
  if (!action) {
    if (const auto* volume =
            std::get_if<playback_input::AdjustVolume>(&command)) {
      audioAdjustVolume(volume->delta);
    }
    return;
  }
  if (const auto editCommand = videoEditCommandForShortcut(*action)) {
    signals.commands.dispatch(VideoEditRequest{*editCommand});
    return;
  }
  switch (*action) {
    case PlaybackAction::Quit:
      requestPlaybackExit(signals, true);
      break;
    case PlaybackAction::Play:
      setPlaybackPaused(view, signals, seekState, false);
      break;
    case PlaybackAction::Pause:
      setPlaybackPaused(view, signals, seekState, true);
      break;
    case PlaybackAction::TogglePause:
      setPlaybackPaused(view, signals, seekState, pauseRequestedByToggle(view));
      break;
    case PlaybackAction::Stop:
      requestPlaybackExit(signals, false);
      break;
    case PlaybackAction::Previous:
      requestTransport(signals, PlaybackTransportCommand::Previous);
      break;
    case PlaybackAction::Next:
      requestTransport(signals, PlaybackTransportCommand::Next);
      break;
    case PlaybackAction::ToggleWindow:
      toggleRequestedLayout(view, signals);
      break;
    case PlaybackAction::TogglePictureInPicture:
    case PlaybackAction::DismissPictureInPicture:
      togglePictureInPicture(view, signals);
      break;
    case PlaybackAction::ToggleFullscreen:
      signals.commands.dispatch(CommandAction::ToggleFullscreen);
      break;
    case PlaybackAction::ToggleRadio:
      cycleRadioFilter(view);
      break;
    case PlaybackAction::Toggle50Hz:
      toggle50Hz(view);
      break;
    case PlaybackAction::ToggleSubtitles:
      toggleSubtitles(view);
      break;
    case PlaybackAction::ToggleAudioTrack:
      toggleAudioTrack(view);
      break;
    case PlaybackAction::SeekBackward:
      sendRelativeSeekRequest(view, signals, seekState, -5000000);
      break;
    case PlaybackAction::SeekForward:
      sendRelativeSeekRequest(view, signals, seekState, 5000000);
      break;
    case PlaybackAction::PreviousFrame:
      requestFrameStep(view, signals, seekState,
                       playback_video_frame_step::Direction::Previous);
      break;
    case PlaybackAction::NextFrame:
      requestFrameStep(view, signals, seekState,
                       playback_video_frame_step::Direction::Next);
      break;
    case PlaybackAction::CopyVideoFrame:
      signals.commands.dispatch(CommandAction::CopyCurrentVideoFrame);
      break;
    case PlaybackAction::VolumeUp:
      audioAdjustVolume(0.10f);
      break;
    case PlaybackAction::VolumeDown:
      audioAdjustVolume(-0.10f);
      break;
    case PlaybackAction::NavigateBackInVideoEditor:
      signals.commands.dispatch(CommandAction::NavigateBack);
      break;
    case PlaybackAction::ExitPlaybackSession:
      requestPlaybackExit(signals, false);
      break;
    case PlaybackAction::DiscardVideoEditsAndExit:
      signals.commands.dispatch(CommandAction::ConfirmPendingExit);
      break;
    case PlaybackAction::CancelVideoEditPrompt:
      signals.commands.dispatch(CommandAction::NavigateBack);
      break;
    case PlaybackAction::ToggleOptions:
    case PlaybackAction::TogglePitchMonitor:
    case PlaybackAction::CloseViewer:
    default:
      break;
  }
}

}  // namespace

void handlePlaybackInputEvent(const PlaybackInputView& view,
                              PlaybackInputSignals& signals,
                              PlaybackSeekGestureState& seekState,
                              const InputEvent& ev) {
  if (signals.commands.contextMenuVisible()) {
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
  const playback_video_edit::Prompt editPrompt =
      signals.commands.videoEditPrompt();
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
    if (signals.commands.videoEditorActive()) {
      shortcutContexts |= kPlaybackShortcutContextVideoEditing;
    }
    if (view.videoWindow.IsPictureInPicture()) {
      shortcutContexts |= kPlaybackShortcutContextPictureInPicture;
    }
  }
  std::optional<PlaybackInputMatch> match =
      matchPlaybackInput(ev, shortcutContexts);
  if (!match) return;
  const PlaybackInputResult playbackResult = match->result;
  dispatchPlaybackInputCommand(view, signals, seekState,
                               std::move(match->command));
  if (playbackResult == PlaybackInputResult::Handled) {
    if (signals.loopStopRequested) {
      return;
    }
    triggerOverlay(view, signals);
    signals.redraw = true;
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
  if (signals.commands.videoEditPrompt() !=
      playback_video_edit::Prompt::None) {
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
      requestPlaybackExit(signals, false);
      break;
    case PlaybackControlCommand::Previous:
      requestTransport(signals, PlaybackTransportCommand::Previous);
      break;
    case PlaybackControlCommand::Next:
      requestTransport(signals, PlaybackTransportCommand::Next);
      break;
  }
  if (signals.loopStopRequested) {
    return;
  }
  triggerOverlay(view, signals);
  signals.redraw = true;
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
  const bool leftPressed = isMouseButtonDown(mouse, MouseButton::Left);
  const bool rightPressed = isMouseButtonDown(mouse, MouseButton::Right);
  const bool dragFromThisSurface =
      seekState.videoEditBoundaryDrag &&
      seekState.videoEditBoundaryDrag->surface == previewSurface;
  const bool progressDragFromThisSurface =
      seekState.progressDragSurface == previewSurface;
  if ((dragFromThisSurface || progressDragFromThisSurface) && !leftPressed) {
    finishVideoEditBoundaryDrag(view, signals, seekState, previewSurface);
    seekState.progressDragSurface.reset();
    commitQueuedSeek(view, signals, seekState);
    signals.redraw = true;
  }
  const playback_video_edit::Prompt editPrompt =
      signals.commands.videoEditPrompt();

  const double pointerX =
      windowEvent && mouse.hasPixelPosition ? mouse.pixelX : mouse.pos.X;
  const double pointerY =
      windowEvent && mouse.hasPixelPosition ? mouse.pixelY : mouse.pos.Y;
  if (mouse.kind == MouseEventKind::Move) {
    triggerOverlay(view, signals);
    signals.redraw = true;
  }

  const bool capturedProgressDrag =
      (dragFromThisSurface || progressDragFromThisSurface) && leftPressed;
  playback_overlay::InteractionHit interactionHit;
  if (windowEvent) {
    interactionHit = view.videoWindow.OverlayHitAt(
        pointerX, pointerY, capturedProgressDrag);
  } else {
    const playback_overlay::InteractionMap& interactions =
        view.frameOutputState.overlayInteractions;
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
  if (rightPressed && mouse.kind == MouseEventKind::Press &&
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
      const int64_t durationUs = view.transport.snapshot().durationUs;
      if (durationUs > 0) {
        request.timelineToleranceUs = std::max<int64_t>(
            1, durationUs / std::max(1, progressHit->units - 1));
      }
    }
    if (dispatchContextMenuInput(signals, request)) return;
  }
  if (signals.commands.contextMenuVisible()) {
    playback_session::ContextMenuInput request;
    request.surface =
        windowEvent ? playback_session::ContextMenuSurface::VideoWindow
                    : playback_session::ContextMenuSurface::Terminal;
    if (mouse.kind == MouseEventKind::VerticalWheel) {
      const int delta = mouse.wheelDelta;
      if (delta != 0) {
        request.kind = playback_session::ContextMenuInputKind::MoveSelection;
        request.selectionDelta = delta > 0 ? -1 : 1;
        dispatchContextMenuInput(signals, request);
      }
      return;
    }
    if (mouse.kind == MouseEventKind::Move) {
      if (contextMenuItemHit) {
        request.kind = playback_session::ContextMenuInputKind::SelectItem;
        request.item = *contextMenuItemHit;
        dispatchContextMenuInput(signals, request);
      }
      return;
    }
    if (leftPressed && mouse.kind == MouseEventKind::Press) {
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
  const bool interactiveHit = progressHit || boundaryHit || controlHit ||
                              contextMenuItemHit;
  if (isPlaybackFullscreenGesture(mouse) && !interactiveHit &&
      editPrompt == playback_video_edit::Prompt::None) {
    signals.commands.dispatch(CommandAction::ToggleFullscreen);
    return;
  }
  if (progressHit) {
    triggerOverlay(view, signals);
    signals.redraw = true;
  }

  const double progressRatio = progressHit ? progressHit->ratio : 0.0;
  const int progressUnits = progressHit ? progressHit->units : 0;

  if (progressHit && leftPressed && !dragFromThisSurface &&
      mouse.kind == MouseEventKind::Press && boundaryHit &&
      signals.commands.videoEditorActive()) {
    setPlaybackPaused(view, signals, seekState, true);
    seekState.pendingVideoEditBoundaryCommit.reset();
    seekState.videoEditBoundaryDrag =
        PlaybackSeekGestureState::VideoEditBoundaryDrag{
            *boundaryHit, previewSurface, -1,
            view.transport.snapshot().latestSeekRequestGeneration};
  } else if (progressHit && leftPressed &&
             mouse.kind == MouseEventKind::Press) {
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
      signals.commands.dispatch(MoveVideoEditBoundary{
          seekState.videoEditBoundaryDrag->boundary, *targetUs});
      const int64_t seekTargetUs = videoEditBoundarySeekTargetUs(
          seekState.videoEditBoundaryDrag->boundary, *targetUs);
      queueSeekRequest(signals, seekState,
                       static_cast<double>(seekTargetUs) / 1000000.0);
      const int64_t durationUs = view.transport.snapshot().durationUs;
      if (durationUs > 0) {
        previewRatio = static_cast<double>(seekTargetUs) /
                       static_cast<double>(durationUs);
      }
    }
    updateOverlayControlHover(signals, -1);
    signals.commands.dispatch(
        TimelinePreviewRequest{previewSurface, previewRatio, progressUnits});
    return;
  }
  const bool seekGesture =
      leftPressed && progressHit &&
      (windowEvent || mouse.kind == MouseEventKind::Press ||
       mouse.kind == MouseEventKind::Move);
  if (progressHit) {
    updateOverlayControlHover(signals, -1);
    signals.commands.dispatch(
        TimelinePreviewRequest{previewSurface, progressRatio, progressUnits});
    if (seekGesture) {
      queuePlaybackSeekToRatio(view, signals, seekState, progressRatio);
    }
    return;
  }
  signals.commands.dispatch(ClearTimelinePreview{previewSurface});
  updateOverlayControlHover(
      signals, controlHit ? playback_overlay::overlayControlToken(*controlHit)
                          : -1);
  if (controlHit) {
    triggerOverlay(view, signals);
  }

  if (leftPressed && mouse.kind == MouseEventKind::Press && controlHit) {
    if (executeOverlayControl(view, signals, seekState, *controlHit)) {
      updateOverlayControlHover(signals, -1);
      if (signals.loopStopRequested) {
        return;
      }
      triggerOverlay(view, signals);
      signals.redraw = true;
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
  signals.commands.dispatch(ClearTimelinePreview{
      playback_video_timeline_preview::PresentationSurface::VideoWindow});
  updateOverlayControlHover(signals, -1);
}

}  // namespace playback_session_input
