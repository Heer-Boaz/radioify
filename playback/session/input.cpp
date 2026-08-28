#include "input.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "playback/overlay/overlay.h"
#include "playback/input/shortcuts.h"
#include "ui_inputlogic.h"

namespace playback_session_input {

void setPlaybackPaused(SessionPort& session,
                       PlaybackSeekGestureState& seekState, bool paused);

namespace {

bool requestTransport(SessionPort& session,
                      PlaybackTransportCommand command) {
  return session.dispatch(TransportRequest{command});
}

bool hasOverlayVisibleWindow(const SessionPort& session) {
  return session.snapshot().playbackControlsVisible;
}

void requestWindowRefresh(SessionPort& session) {
  session.dispatch(CommandAction::RequestWindowPresent);
}

void updateOverlayControlHover(SessionPort& session, int nextHover) {
  session.dispatch(SetOverlayControlHover{nextHover});
}

double playbackDurationSec(const SessionPort& session) {
  const SessionSnapshot state = session.snapshot();
  const int64_t durationUs = state.transport.durationUs;
  if (durationUs > 0) {
    return static_cast<double>(durationUs) / 1000000.0;
  }
  return state.audioDurationSec;
}

double clampPlaybackSeekTarget(const SessionPort& session,
                               double targetSec) {
  double target = std::isfinite(targetSec) ? targetSec : 0.0;
  target = std::max(0.0, target);
  const double totalSec = playbackDurationSec(session);
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

bool pauseRequestedByToggle(const SessionPort& session) {
  const TransportSnapshot transport = session.snapshot().transport;
  return playback_session_state::toggleRequestsPause(
      transport.state, transport.ended);
}

bool queuePlaybackSeekToRatio(SessionPort& session,
                              PlaybackSeekGestureState& seekState,
                              double ratio) {
  const double totalSec = playbackDurationSec(session);
  if (!(totalSec > 0.0) || !std::isfinite(totalSec)) {
    return false;
  }
  queueSeekRequest(session, seekState, std::clamp(ratio, 0.0, 1.0) * totalSec);
  return true;
}

std::optional<int64_t> playbackTimelineTargetForRatio(
    const SessionPort& session, double ratio) {
  const int64_t durationUs = session.snapshot().transport.durationUs;
  if (durationUs <= 0 || !std::isfinite(ratio)) return std::nullopt;
  return static_cast<int64_t>(std::llround(
      std::clamp(ratio, 0.0, 1.0) * static_cast<double>(durationUs)));
}

void triggerOverlay(SessionPort& session) {
  const TransportSnapshot transport = session.snapshot().transport;
  const bool extended = transport.state == PlaybackSessionState::Paused ||
                        transport.state == PlaybackSessionState::Ended ||
                        transport.seekPending;
  constexpr auto kProgressOverlayTimeout = std::chrono::milliseconds(1750);
  constexpr auto kProgressOverlayExtendedTimeout =
      std::chrono::milliseconds(2500);
  const auto timeout = extended ? kProgressOverlayExtendedTimeout
                                : kProgressOverlayTimeout;
  session.dispatch(ShowPlaybackControls{timeout});
  requestWindowRefresh(session);
}

void requestPlaybackExit(SessionPort& session, bool quitApp) {
  session.dispatch(PlaybackExitRequest{quitApp});
}

void refreshPlaybackInputDisplay(SessionPort& session) {
  session.dispatch(CommandAction::RequestFrameRefresh);
}

void clearQueuedSeek(PlaybackSeekGestureState& seekState) {
  seekState.queuedSeekTargetSec = -1.0;
  seekState.seekQueued = false;
}

void markSeekSent(SessionPort& session,
                  PlaybackSeekGestureState& seekState) {
  seekState.lastSeekSentTime = std::chrono::steady_clock::now();
  clearQueuedSeek(seekState);
  refreshPlaybackInputDisplay(session);
}

void refreshFrameStepRequestDisplay(SessionPort& session) {
  refreshPlaybackInputDisplay(session);
  requestWindowRefresh(session);
}

void commitQueuedSeek(SessionPort& session,
                      PlaybackSeekGestureState& seekState) {
  double queuedTargetSec = 0.0;
  if (readQueuedSeekTargetSec(seekState, &queuedTargetSec)) {
    sendSeekRequest(session, seekState, queuedTargetSec);
  }
}

void finishVideoEditBoundaryDrag(
    SessionPort& session, PlaybackSeekGestureState& seekState,
    playback_video_timeline_preview::PresentationSurface surface) {
  if (!seekState.videoEditBoundaryDrag ||
      seekState.videoEditBoundaryDrag->surface != surface) {
    return;
  }
  const PlaybackSeekGestureState::VideoEditBoundaryDrag completed =
      *seekState.videoEditBoundaryDrag;
  commitQueuedSeek(session, seekState);
  const TransportSnapshot transport = session.snapshot().transport;
  if (completed.targetTimelineUs >= 0 &&
      transport.latestSeekRequestGeneration >
          completed.seekGenerationAtStart) {
    seekState.pendingVideoEditBoundaryCommit =
        PlaybackSeekGestureState::PendingVideoEditBoundaryCommit{
            completed.boundary, transport.latestSeekRequestGeneration};
  }
  seekState.videoEditBoundaryDrag.reset();
}

void sendRelativeSeekRequest(SessionPort& session,
                             PlaybackSeekGestureState& seekState,
                             int64_t deltaUs) {
  commitQueuedSeek(session, seekState);
  if (!session.dispatch(SeekBy{deltaUs})) {
    return;
  }
  markSeekSent(session, seekState);
}

bool toggleRequestedLayout(SessionPort& session) {
  return session.dispatch(CommandAction::ToggleWindowPresentation);
}

bool toggleSubtitles(SessionPort& session) {
  return session.dispatch(CommandAction::ToggleSubtitles);
}

bool toggleAudioTrack(SessionPort& session) {
  return session.dispatch(CommandAction::CycleAudioTrack);
}

bool cycleRadioFilter(SessionPort& session) {
  return session.dispatch(CommandAction::ToggleRadio);
}

bool toggle50Hz(SessionPort& session) {
  return session.dispatch(CommandAction::Toggle50Hz);
}

bool togglePictureInPicture(SessionPort& session) {
  return session.dispatch(CommandAction::TogglePictureInPicture);
}

bool requestFrameStep(SessionPort& session,
                      PlaybackSeekGestureState& seekState,
                      playback_video_frame_step::Direction direction) {
  commitQueuedSeek(session, seekState);

  if (!session.dispatch(StepFrame{direction})) {
    return false;
  }
  refreshFrameStepRequestDisplay(session);
  return true;
}

bool executeOverlayControl(SessionPort& session,
                           PlaybackSeekGestureState& seekState,
                           playback_overlay::OverlayControlId control) {
  const playback_overlay::OverlayControlIntent intent =
      playback_overlay::intentForOverlayControl(control);
  if (const auto* edit =
          std::get_if<playback_video_edit::Command>(&intent)) {
    return session.dispatch(VideoEditRequest{*edit});
  }

  using Action = playback_overlay::OverlayAction;
  switch (std::get<Action>(intent)) {
    case Action::Previous:
      return requestTransport(session, PlaybackTransportCommand::Previous);
    case Action::TogglePlayPause:
      setPlaybackPaused(session, seekState,
                        pauseRequestedByToggle(session));
      return true;
    case Action::Next:
      return requestTransport(session, PlaybackTransportCommand::Next);
    case Action::ToggleRadio:
      return cycleRadioFilter(session);
    case Action::Toggle50Hz:
      return toggle50Hz(session);
    case Action::CycleAudioTrack:
      return toggleAudioTrack(session);
    case Action::ToggleSubtitles:
      return toggleSubtitles(session);
    case Action::TogglePictureInPicture:
      return togglePictureInPicture(session);
    case Action::WaitForVideoEditExport:
      return session.dispatch(CommandAction::WaitForVideoEditExportAndExit);
    case Action::ConfirmPendingExit:
      return session.dispatch(CommandAction::ConfirmPendingExit);
    case Action::CancelPendingExit:
      return session.dispatch(CommandAction::CancelPendingExit);
    case Action::ConfirmMediaTaskCancellation:
      return session.dispatch(
          CommandAction::ConfirmMediaTaskCancellation);
    case Action::DismissMediaTaskCancellation:
      return session.dispatch(
          CommandAction::DismissMediaTaskCancellation);
  }
  return false;
}

bool dispatchContextMenuInput(
    SessionPort& session,
    const playback_session::ContextMenuInput& request) {
  if (!session.dispatch(ContextMenuRequest{request})) return false;
  updateOverlayControlHover(session, -1);
  return true;
}

}  // namespace

bool isOverlayVisible(const SessionPort& session) {
  return hasOverlayVisibleWindow(session);
}

void sendSeekRequest(SessionPort& session,
                     PlaybackSeekGestureState& seekState, double targetSec) {
  targetSec = clampPlaybackSeekTarget(session, targetSec);
  int64_t targetUs =
      static_cast<int64_t>(std::llround(targetSec * 1000000.0));
  if (!session.dispatch(SeekTo{targetUs})) {
    return;
  }
  markSeekSent(session, seekState);
}

void queueSeekRequest(SessionPort& session,
                      PlaybackSeekGestureState& seekState, double targetSec) {
  seekState.queuedSeekTargetSec = targetSec;
  seekState.seekQueued = true;
  refreshPlaybackInputDisplay(session);
}

void setPlaybackPaused(SessionPort& session,
                       PlaybackSeekGestureState& seekState, bool paused) {
  commitQueuedSeek(session, seekState);
  session.dispatch(SetPaused{paused});
}

namespace {

void dispatchPlaybackInputCommand(
    SessionPort& session, PlaybackSeekGestureState& seekState,
    playback_input::Command command) {
  const auto* action = std::get_if<PlaybackAction>(&command);
  if (!action) {
    if (const auto* volume =
            std::get_if<playback_input::AdjustVolume>(&command)) {
      session.dispatch(AdjustVolume{volume->delta});
    }
    return;
  }
  if (const auto editCommand = videoEditCommandForShortcut(*action)) {
    session.dispatch(VideoEditRequest{*editCommand});
    return;
  }
  switch (*action) {
    case PlaybackAction::Quit:
      requestPlaybackExit(session, true);
      break;
    case PlaybackAction::Play:
      setPlaybackPaused(session, seekState, false);
      break;
    case PlaybackAction::Pause:
      setPlaybackPaused(session, seekState, true);
      break;
    case PlaybackAction::TogglePause:
      setPlaybackPaused(session, seekState, pauseRequestedByToggle(session));
      break;
    case PlaybackAction::Stop:
      requestPlaybackExit(session, false);
      break;
    case PlaybackAction::Previous:
      requestTransport(session, PlaybackTransportCommand::Previous);
      break;
    case PlaybackAction::Next:
      requestTransport(session, PlaybackTransportCommand::Next);
      break;
    case PlaybackAction::ToggleWindow:
      toggleRequestedLayout(session);
      break;
    case PlaybackAction::TogglePictureInPicture:
    case PlaybackAction::DismissPictureInPicture:
      togglePictureInPicture(session);
      break;
    case PlaybackAction::ToggleFullscreen:
      session.dispatch(CommandAction::ToggleFullscreen);
      break;
    case PlaybackAction::ToggleRadio:
      cycleRadioFilter(session);
      break;
    case PlaybackAction::Toggle50Hz:
      toggle50Hz(session);
      break;
    case PlaybackAction::ToggleSubtitles:
      toggleSubtitles(session);
      break;
    case PlaybackAction::ToggleAudioTrack:
      toggleAudioTrack(session);
      break;
    case PlaybackAction::SeekBackward:
      sendRelativeSeekRequest(session, seekState, -5000000);
      break;
    case PlaybackAction::SeekForward:
      sendRelativeSeekRequest(session, seekState, 5000000);
      break;
    case PlaybackAction::PreviousFrame:
      requestFrameStep(session, seekState,
                       playback_video_frame_step::Direction::Previous);
      break;
    case PlaybackAction::NextFrame:
      requestFrameStep(session, seekState,
                       playback_video_frame_step::Direction::Next);
      break;
    case PlaybackAction::CopyVideoFrame:
      session.dispatch(CommandAction::CopyCurrentVideoFrame);
      break;
    case PlaybackAction::VolumeUp:
      session.dispatch(AdjustVolume{0.10f});
      break;
    case PlaybackAction::VolumeDown:
      session.dispatch(AdjustVolume{-0.10f});
      break;
    case PlaybackAction::NavigateBackInVideoEditor:
      session.dispatch(CommandAction::NavigateBack);
      break;
    case PlaybackAction::ExitPlaybackSession:
      requestPlaybackExit(session, false);
      break;
    case PlaybackAction::DiscardVideoEditsAndExit:
      session.dispatch(CommandAction::ConfirmPendingExit);
      break;
    case PlaybackAction::CancelVideoEditPrompt:
      session.dispatch(CommandAction::NavigateBack);
      break;
    case PlaybackAction::SelectPreviousMediaTaskCancellationAction:
      session.dispatch(
          CommandAction::SelectPreviousMediaTaskCancellationAction);
      break;
    case PlaybackAction::SelectNextMediaTaskCancellationAction:
      session.dispatch(
          CommandAction::SelectNextMediaTaskCancellationAction);
      break;
    case PlaybackAction::ActivateMediaTaskCancellationAction:
      session.dispatch(
          CommandAction::ActivateSelectedMediaTaskCancellationAction);
      break;
    case PlaybackAction::DismissMediaTaskCancellation:
      session.dispatch(CommandAction::DismissMediaTaskCancellation);
      break;
    case PlaybackAction::ToggleOptions:
    case PlaybackAction::TogglePitchMonitor:
    case PlaybackAction::CloseViewer:
    default:
      break;
  }
}

}  // namespace

void handlePlaybackInputEvent(SessionPort& session,
                              PlaybackSeekGestureState& seekState,
                              const InputEvent& ev) {
  if (session.snapshot().contextMenuVisible) {
    playback_session::ContextMenuInput request;
    if (ev.type == InputEvent::Type::Action &&
        ev.action == InputAction::Back) {
      request.kind = playback_session::ContextMenuInputKind::Dismiss;
      dispatchContextMenuInput(session, request);
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
      dispatchContextMenuInput(session, request);
      return;
    }
  }
  const SessionSnapshot initialState = session.snapshot();
  const playback_video_edit::Prompt editPrompt =
      initialState.videoEditPrompt;
  uint32_t shortcutContexts = 0;
  if (initialState.mediaTaskCancellationPrompt) {
    shortcutContexts =
        kPlaybackShortcutContextMediaTaskCancellationConfirmation;
  } else if (editPrompt == playback_video_edit::Prompt::LeaveEditMode) {
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
    const SessionSnapshot& state = initialState;
    if (state.videoEditorActive) {
      shortcutContexts |= kPlaybackShortcutContextVideoEditing;
    }
    if (state.pictureInPicture) {
      shortcutContexts |= kPlaybackShortcutContextPictureInPicture;
    }
  }
  std::optional<PlaybackInputMatch> match =
      matchPlaybackInput(ev, shortcutContexts);
  if (!match) return;
  const PlaybackInputResult playbackResult = match->result;
  dispatchPlaybackInputCommand(session, seekState,
                               std::move(match->command));
  if (playbackResult == PlaybackInputResult::Handled) {
    if (session.snapshot().stopRequested) {
      return;
    }
    triggerOverlay(session);
    session.dispatch(CommandAction::RequestRedraw);
    return;
  }
  if (playbackResult == PlaybackInputResult::HandledWithoutOverlayRefresh) {
    return;
  }
}

void handlePlaybackControlCommand(SessionPort& session,
                                  PlaybackSeekGestureState& seekState,
                                  PlaybackControlCommand command) {
  const SessionSnapshot state = session.snapshot();
  if (state.mediaTaskCancellationPrompt ||
      state.videoEditPrompt != playback_video_edit::Prompt::None) {
    return;
  }
  switch (command) {
    case PlaybackControlCommand::Play:
      setPlaybackPaused(session, seekState, false);
      break;
    case PlaybackControlCommand::Pause:
      setPlaybackPaused(session, seekState, true);
      break;
    case PlaybackControlCommand::TogglePause: {
      setPlaybackPaused(session, seekState, pauseRequestedByToggle(session));
      break;
    }
    case PlaybackControlCommand::Stop:
      requestPlaybackExit(session, false);
      break;
    case PlaybackControlCommand::Previous:
      requestTransport(session, PlaybackTransportCommand::Previous);
      break;
    case PlaybackControlCommand::Next:
      requestTransport(session, PlaybackTransportCommand::Next);
      break;
  }
  if (session.snapshot().stopRequested) {
    return;
  }
  triggerOverlay(session);
  session.dispatch(CommandAction::RequestRedraw);
}

void handlePlaybackMouseEvent(SessionPort& session,
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
    finishVideoEditBoundaryDrag(session, seekState, previewSurface);
    seekState.progressDragSurface.reset();
    commitQueuedSeek(session, seekState);
    session.dispatch(CommandAction::RequestRedraw);
  }
  const SessionSnapshot interactionState = session.snapshot();
  const playback_video_edit::Prompt editPrompt =
      interactionState.videoEditPrompt;

  const double pointerX =
      windowEvent && mouse.hasPixelPosition ? mouse.pixelX : mouse.pos.X;
  const double pointerY =
      windowEvent && mouse.hasPixelPosition ? mouse.pixelY : mouse.pos.Y;
  if (mouse.kind == MouseEventKind::Move) {
    triggerOverlay(session);
    session.dispatch(CommandAction::RequestRedraw);
  }

  const bool capturedProgressDrag =
      (dragFromThisSurface || progressDragFromThisSurface) && leftPressed;
  const bool transformedTerminalPointer =
      !windowEvent && mouse.hasPixelPosition;
  const playback_overlay::InteractionHit interactionHit = session.hitTest({
      previewSurface,
      transformedTerminalPointer ? mouse.pixelX : pointerX,
      transformedTerminalPointer ? mouse.pixelY : pointerY,
      transformedTerminalPointer ? std::max(1.0, mouse.unitWidth) : 1.0,
      transformedTerminalPointer ? std::max(1.0, mouse.unitHeight) : 1.0,
      capturedProgressDrag,
  });
  const auto& progressHit = interactionHit.progressBar;
  const auto& boundaryHit = interactionHit.editBoundary;
  const auto& controlHit = interactionHit.control;
  const auto& contextMenuItemHit = interactionHit.contextMenuItem;
  if (rightPressed && mouse.kind == MouseEventKind::Press &&
      !interactionState.mediaTaskCancellationPrompt &&
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
          playbackTimelineTargetForRatio(session, progressHit->ratio);
      const int64_t durationUs = session.snapshot().transport.durationUs;
      if (durationUs > 0) {
        request.timelineToleranceUs = std::max<int64_t>(
            1, durationUs / std::max(1, progressHit->units - 1));
      }
    }
    if (dispatchContextMenuInput(session, request)) return;
  }
  if (session.snapshot().contextMenuVisible) {
    playback_session::ContextMenuInput request;
    request.surface =
        windowEvent ? playback_session::ContextMenuSurface::VideoWindow
                    : playback_session::ContextMenuSurface::Terminal;
    if (mouse.kind == MouseEventKind::VerticalWheel) {
      const int delta = mouse.wheelDelta;
      if (delta != 0) {
        request.kind = playback_session::ContextMenuInputKind::MoveSelection;
        request.selectionDelta = delta > 0 ? -1 : 1;
        dispatchContextMenuInput(session, request);
      }
      return;
    }
    if (mouse.kind == MouseEventKind::Move) {
      if (contextMenuItemHit) {
        request.kind = playback_session::ContextMenuInputKind::SelectItem;
        request.item = *contextMenuItemHit;
        dispatchContextMenuInput(session, request);
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
      dispatchContextMenuInput(session, request);
      return;
    }
    return;
  }
  const playback_session_pointer::Interaction controlPointer =
      seekState.overlayControlPointer.handle(mouse, previewSurface,
                                             controlHit);
  if (controlPointer.activated) {
    if (executeOverlayControl(session, seekState,
                              *controlPointer.activated)) {
      updateOverlayControlHover(session, -1);
      if (session.snapshot().stopRequested) return;
      triggerOverlay(session);
      session.dispatch(CommandAction::RequestRedraw);
    }
    return;
  }
  if (controlPointer.captured ||
      interactionState.mediaTaskCancellationPrompt) {
    updateOverlayControlHover(
        session,
        controlHit ? playback_overlay::overlayControlToken(*controlHit) : -1);
    return;
  }
  const bool interactiveHit = progressHit || boundaryHit || controlHit ||
                              contextMenuItemHit;
  if (isPlaybackFullscreenGesture(mouse) && !interactiveHit &&
      editPrompt == playback_video_edit::Prompt::None) {
    session.dispatch(CommandAction::ToggleFullscreen);
    return;
  }
  if (progressHit) {
    triggerOverlay(session);
    session.dispatch(CommandAction::RequestRedraw);
  }

  const double progressRatio = progressHit ? progressHit->ratio : 0.0;
  const int progressUnits = progressHit ? progressHit->units : 0;

  if (progressHit && leftPressed && !dragFromThisSurface &&
      mouse.kind == MouseEventKind::Press && boundaryHit &&
      session.snapshot().videoEditorActive) {
    setPlaybackPaused(session, seekState, true);
    seekState.pendingVideoEditBoundaryCommit.reset();
    seekState.videoEditBoundaryDrag =
        PlaybackSeekGestureState::VideoEditBoundaryDrag{
            *boundaryHit, previewSurface, -1,
            session.snapshot().transport.latestSeekRequestGeneration};
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
            playbackTimelineTargetForRatio(session, progressRatio)) {
      seekState.videoEditBoundaryDrag->targetTimelineUs = *targetUs;
      session.dispatch(MoveVideoEditBoundary{
          seekState.videoEditBoundaryDrag->boundary, *targetUs});
      const int64_t seekTargetUs = videoEditBoundarySeekTargetUs(
          seekState.videoEditBoundaryDrag->boundary, *targetUs);
      queueSeekRequest(session, seekState,
                       static_cast<double>(seekTargetUs) / 1000000.0);
      const int64_t durationUs = session.snapshot().transport.durationUs;
      if (durationUs > 0) {
        previewRatio = static_cast<double>(seekTargetUs) /
                       static_cast<double>(durationUs);
      }
    }
    updateOverlayControlHover(session, -1);
    session.dispatch(
        TimelinePreviewRequest{previewSurface, previewRatio, progressUnits});
    return;
  }
  const bool seekGesture =
      leftPressed && progressHit &&
      (windowEvent || mouse.kind == MouseEventKind::Press ||
       mouse.kind == MouseEventKind::Move);
  if (progressHit) {
    updateOverlayControlHover(session, -1);
    session.dispatch(
        TimelinePreviewRequest{previewSurface, progressRatio, progressUnits});
    if (seekGesture) {
      queuePlaybackSeekToRatio(session, seekState, progressRatio);
    }
    return;
  }
  session.dispatch(ClearTimelinePreview{previewSurface});
  updateOverlayControlHover(
      session, controlHit ? playback_overlay::overlayControlToken(*controlHit)
                          : -1);
  if (controlHit) {
    triggerOverlay(session);
  }

  if (leftPressed && windowEvent) {
    return;
  }
}

void handlePlaybackPointerLeave(SessionPort& session,
                                PlaybackSeekGestureState& seekState) {
  finishVideoEditBoundaryDrag(
      session, seekState,
      playback_video_timeline_preview::PresentationSurface::VideoWindow);
  seekState.progressDragSurface.reset();
  seekState.overlayControlPointer.reset();
  commitQueuedSeek(session, seekState);
  session.dispatch(ClearTimelinePreview{
      playback_video_timeline_preview::PresentationSurface::VideoWindow});
  updateOverlayControlHover(session, -1);
}

}  // namespace playback_session_input
