#include "input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "audioplayback.h"
#include "playback/video/player.h"
#include "playback/video/state/machine.h"
#include "playback/video/edit/overlay_model.h"
#include "playback/input/shortcuts.h"
#include "playback/session/osd_timeline.h"
#include "handoff.h"
#include "playback/video/subtitle/manager.h"
#include "ui_helpers.h"
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
                           const playback_overlay::PlaybackOverlayState& state,
                           int controlIndex) {
  std::vector<playback_overlay::OverlayControlSpec> specs =
      playback_overlay::buildOverlayControlSpecs(state, -1);
  if (controlIndex < 0 || controlIndex >= static_cast<int>(specs.size())) {
    return false;
  }
  const auto& spec = specs[static_cast<size_t>(controlIndex)];
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
  const auto editAction = [&](PlaybackShortcutAction action) {
    return signals.handleVideoEditorAction &&
           signals.handleVideoEditorAction(action);
  };
  actions.editMarkIn = [&]() {
    return editAction(PlaybackShortcutAction::SetVideoEditIn);
  };
  actions.editMarkOut = [&]() {
    return editAction(PlaybackShortcutAction::SetVideoEditOut);
  };
  actions.editRippleDelete = [&]() {
    return editAction(
        PlaybackShortcutAction::RippleDeleteVideoEditSelection);
  };
  actions.editTrim = [&]() {
    return editAction(PlaybackShortcutAction::TrimVideoEditSelection);
  };
  actions.editUndo = [&]() {
    return editAction(PlaybackShortcutAction::UndoVideoEdit);
  };
  actions.editRedo = [&]() {
    return editAction(PlaybackShortcutAction::RedoVideoEdit);
  };
  actions.editReset = [&]() {
    return editAction(PlaybackShortcutAction::ResetVideoEdits);
  };
  actions.editExport = [&]() {
    return editAction(PlaybackShortcutAction::ExportVideoEdits);
  };
  actions.editDone = [&]() {
    return editAction(PlaybackShortcutAction::ExitVideoEditor);
  };
  actions.editDiscardAndExit = [&]() {
    return editAction(PlaybackShortcutAction::DiscardVideoEditsAndExit);
  };
  actions.editCancelExit = [&]() {
    return editAction(PlaybackShortcutAction::CancelVideoEditExit);
  };
  return playback_overlay::dispatchOverlayControl(spec.id, actions);
}

playback_overlay::PlaybackOverlayInputs buildPlaybackMouseOverlayInputs(
    const PlaybackInputView& view,
    const PlaybackSeekGestureState& seekState,
    const PlaybackInputSignals& signals) {
  playback_overlay::PlaybackOverlayInputs inputs;
  inputs.windowTitle = *view.windowTitle;
  inputs.audioOk = *view.audioOk;
  inputs.playPauseAvailable =
      *view.playbackState == PlaybackSessionState::Active ||
      *view.playbackState == PlaybackSessionState::Paused ||
      *view.playbackState == PlaybackSessionState::Ended;
  inputs.audioSupports50HzToggle =
      inputs.audioOk && audioSupports50HzToggle();
  inputs.canPlayPrevious = signals.requestTransportCommand != nullptr;
  inputs.canPlayNext = signals.requestTransportCommand != nullptr;
  inputs.radioEnabled = audioIsRadioEnabled();
  inputs.radioLabel = std::string(audioGetRadioFilterLabel());
  inputs.hz50Enabled = audioIs50HzEnabled();
  inputs.canCycleAudioTracks =
      inputs.audioOk && view.player->canCycleAudioTracks();
  inputs.activeAudioTrackLabel =
      inputs.audioOk ? view.player->activeAudioTrackLabel() : "N/A";
  inputs.subtitleManager = view.subtitleManager;
  inputs.hasSubtitles = view.hasSubtitles;
  inputs.subtitlesEnabled =
      view.enableSubtitlesShared->load(std::memory_order_relaxed);
  const PlayerTimelineSnapshot timeline = view.player->timelineSnapshot();
  const int64_t currentUs = timeline.positionUs;
  inputs.subtitleClockUs = currentUs;
  inputs.seekingOverlay =
      readQueuedSeekTargetSec(seekState, nullptr) || timeline.seekPending();
  inputs.displaySec = std::max(
      0.0, static_cast<double>(currentUs) / 1000000.0);
  const int64_t durationUs = view.player->durationUs();
  inputs.totalSec = durationUs > 0
                        ? static_cast<double>(durationUs) / 1000000.0
                        : (inputs.audioOk ? audioGetTotalSec() : -1.0);
  if (inputs.totalSec > 0.0) {
    inputs.displaySec = std::clamp(inputs.displaySec, 0.0, inputs.totalSec);
  }
  double queuedSeekTargetSec = 0.0;
  if (inputs.totalSec > 0.0 && std::isfinite(inputs.totalSec) &&
      readQueuedSeekTargetSec(seekState, &queuedSeekTargetSec)) {
    inputs.displaySec =
        std::clamp(queuedSeekTargetSec, 0.0, inputs.totalSec);
  }
  inputs.volPct = static_cast<int>(std::round(audioGetVolume() * 100.0f));
  inputs.osd.controlsVisible = isOverlayVisible(signals);
  inputs.paused =
      *view.playbackState == PlaybackSessionState::Paused ||
      *view.playbackState == PlaybackSessionState::Ended ||
      view.player->isEnded() ||
      playback_video_state_machine::project(view.player->state()).transport ==
          playback_video_state_machine::TransportState::Paused;
  inputs.audioFinished = inputs.audioOk && audioIsFinished();
  inputs.pictureInPictureAvailable =
      signals.togglePictureInPicture != nullptr || view.videoWindow->IsOpen();
  inputs.pictureInPictureActive =
      view.videoWindow->IsOpen() &&
      view.videoWindow->IsPictureInPicture();
  inputs.subtitleRenderError = view.videoWindow->GetSubtitleRenderError();
  inputs.screenWidth = view.screen->width();
  inputs.screenHeight = view.screen->height();
  inputs.windowWidth =
      view.videoWindow->IsOpen() ? view.videoWindow->GetWidth() : 0;
  inputs.windowHeight =
      view.videoWindow->IsOpen() ? view.videoWindow->GetHeight() : 0;
  inputs.artTop = 0;
  inputs.progressBarX = view.frameOutputState->progressBarX;
  inputs.progressBarY = view.frameOutputState->progressBarY;
  inputs.progressBarWidth = view.frameOutputState->progressBarWidth;
  if (view.videoEdit) inputs.videoEdit = *view.videoEdit;
  if (view.videoEditExport) inputs.videoEditExport = *view.videoEditExport;
  return inputs;
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
    switch (action) {
      case PlaybackShortcutAction::TogglePictureInPicture:
        togglePictureInPicture(view, signals);
        break;
      case PlaybackShortcutAction::ExitPlaybackSession:
        requestPlaybackExit(view, signals, false);
        break;
      case PlaybackShortcutAction::ToggleVideoEditor:
        if (!signals.videoEditorActive || !signals.videoEditorActive()) {
          setPlaybackPaused(view, signals, seekState, true);
        }
        if (signals.handleVideoEditorAction) {
          signals.handleVideoEditorAction(action);
        }
        break;
      case PlaybackShortcutAction::ExitVideoEditor:
      case PlaybackShortcutAction::SetVideoEditIn:
      case PlaybackShortcutAction::SetVideoEditOut:
      case PlaybackShortcutAction::RippleDeleteVideoEditSelection:
      case PlaybackShortcutAction::TrimVideoEditSelection:
      case PlaybackShortcutAction::UndoVideoEdit:
      case PlaybackShortcutAction::RedoVideoEdit:
      case PlaybackShortcutAction::ResetVideoEdits:
      case PlaybackShortcutAction::ExportVideoEdits:
      case PlaybackShortcutAction::DiscardVideoEditsAndExit:
      case PlaybackShortcutAction::CancelVideoEditExit:
        if (signals.handleVideoEditorAction) {
          signals.handleVideoEditorAction(action);
        }
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

  uint32_t shortcutContexts = 0;
  if (signals.videoEditExitConfirmationActive &&
      signals.videoEditExitConfirmationActive()) {
    shortcutContexts = kPlaybackShortcutContextVideoEditExitConfirmation;
  } else {
    shortcutContexts = kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextPlaybackSession |
                       kPlaybackShortcutContextVideoPlayback;
    if (signals.videoEditorActive && signals.videoEditorActive()) {
      shortcutContexts |= kPlaybackShortcutContextVideoEditing;
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
  if (signals.videoEditExitConfirmationActive &&
      signals.videoEditExitConfirmationActive()) {
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
  MouseEvent hitMouse = mouse;
  bool overlayVisibleForHitTest = isOverlayVisible(signals);
  const bool windowOriginEvent = isWindowMouseEvent(mouse);
  const auto previewSurface =
      windowOriginEvent
          ? playback_video_timeline_preview::PresentationSurface::VideoWindow
          : playback_video_timeline_preview::PresentationSurface::Terminal;
  const bool leftPressed =
      (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
  const bool dragFromThisSurface =
      seekState.videoEditBoundaryDrag &&
      seekState.videoEditBoundaryDragFromWindow == windowOriginEvent;
  if (dragFromThisSurface && !leftPressed) {
    seekState.videoEditBoundaryDrag.reset();
    seekState.videoEditBoundaryDragFromWindow = false;
    commitQueuedSeek(view, signals, seekState);
    *signals.redraw = true;
  }
  const bool terminalAsciiProgress =
      !windowOriginEvent && isAsciiPlaybackMode(view.currentMode);
  const bool editExitConfirmation =
      view.videoEdit && view.videoEdit->exitConfirmation;
  const playback_frame_output::FrameOutputState* progressOutputState =
      terminalAsciiProgress && !editExitConfirmation ? view.frameOutputState
                                                     : nullptr;
  int textGridHitTestCols = 0;
  int textGridHitTestRows = 0;
  bool windowEvent = windowOriginEvent;
  if (windowEvent && view.videoWindow->IsPictureInPicture() &&
      view.videoWindow->IsTextGridPresentationEnabled()) {
    int gridCols = 0;
    int gridRows = 0;
    view.videoWindow->GetTextGridSize(gridCols, gridRows);
    const int winW = view.videoWindow->GetWidth();
    const int winH = view.videoWindow->GetHeight();
    int cellW = 1;
    int cellH = 1;
    view.videoWindow->GetTextGridCellSize(cellW, cellH);
    if (gridCols > 0 && gridRows > 0 && winW > 0 && winH > 0) {
      const GpuTextGridViewport gridViewport = fitGpuTextGridViewport(
          winW, winH, gridCols, gridRows, cellW, cellH);
      const int pixelX = mouse.hasPixelPosition ? mouse.pixelX : mouse.pos.X;
      const int pixelY = mouse.hasPixelPosition ? mouse.pixelY : mouse.pos.Y;
      const int rawLocalPixelX = pixelX - gridViewport.x;
      const int rawLocalPixelY = pixelY - gridViewport.y;
      const bool insideGrid =
          rawLocalPixelX >= 0 && rawLocalPixelY >= 0 &&
          rawLocalPixelX < gridViewport.width &&
          rawLocalPixelY < gridViewport.height;
      if (insideGrid || (dragFromThisSurface && leftPressed)) {
        const int localPixelX =
            std::clamp(rawLocalPixelX, 0, gridViewport.width - 1);
        const int localPixelY =
            std::clamp(rawLocalPixelY, 0, gridViewport.height - 1);
        hitMouse.pos.X = static_cast<SHORT>(std::clamp(
            static_cast<int>((static_cast<int64_t>(localPixelX) * gridCols) /
                             gridViewport.width),
            0, gridCols - 1));
        hitMouse.pos.Y = static_cast<SHORT>(std::clamp(
            static_cast<int>((static_cast<int64_t>(localPixelY) * gridRows) /
                             gridViewport.height),
            0, gridRows - 1));
        clearWindowMouseEvent(hitMouse);
        hitMouse.hasPixelPosition = true;
        hitMouse.pixelX = localPixelX;
        hitMouse.pixelY = localPixelY;
        hitMouse.unitWidth =
            static_cast<double>(gridViewport.width) /
            static_cast<double>(gridCols);
        hitMouse.unitHeight =
            static_cast<double>(gridViewport.height) /
            static_cast<double>(gridRows);
        progressOutputState = editExitConfirmation
                                  ? nullptr
                                  : view.textGridPresentationOutputState;
        overlayVisibleForHitTest = true;
        textGridHitTestCols = gridCols;
        textGridHitTestRows = gridRows;
      } else {
        clearWindowMouseEvent(hitMouse);
        progressOutputState = nullptr;
        overlayVisibleForHitTest = false;
      }
    }
  }

  if (playback_overlay::isBackMousePressed(mouse)) {
    if (editExitConfirmation && signals.handleVideoEditorAction) {
      signals.handleVideoEditorAction(
          PlaybackShortcutAction::CancelVideoEditExit);
    } else {
      requestPlaybackExit(view, signals, false);
    }
    return;
  }
  if (mouse.eventFlags == MOUSE_MOVED) {
    triggerOverlay(view, signals);
    *signals.redraw = true;
  }

  windowEvent = isWindowMouseEvent(hitMouse);
  int windowTextCellW = 1;
  int windowTextCellH = 1;
  if (windowEvent) {
    view.videoWindow->GetTextGridCellSize(windowTextCellW, windowTextCellH);
  }

  double progressRatio = 0.0;
  bool progressHit = false;
  int progressUnits = 0;
  ProgressBarHitTestInput progressGeometry;
  bool hasProgressGeometry = false;
  if (progressOutputState) {
    double unitWidth = 1.0;
    double unitHeight = 1.0;
    if (hitMouse.hasPixelPosition) {
      unitWidth = hitMouse.unitWidth;
      unitHeight = hitMouse.unitHeight;
      if (terminalAsciiProgress) {
        unitWidth = view.screen->cellPixelWidth();
        unitHeight = view.screen->cellPixelHeight();
      }
    }
    progressGeometry.x =
        hitMouse.hasPixelPosition ? hitMouse.pixelX : hitMouse.pos.X;
    progressGeometry.y =
        hitMouse.hasPixelPosition ? hitMouse.pixelY : hitMouse.pos.Y;
    progressGeometry.barX = progressOutputState->progressBarX;
    progressGeometry.barY = progressOutputState->progressBarY;
    progressGeometry.barWidth = progressOutputState->progressBarWidth;
    progressGeometry.unitWidth = unitWidth;
    progressGeometry.unitHeight = unitHeight;
    hasProgressGeometry = true;
    progressUnits = progressOutputState->progressBarWidth;
    progressHit = progressBarRatioAt(progressGeometry, &progressRatio);
  } else if (windowEvent && !editExitConfirmation) {
    progressHit = playback_overlay::windowOverlayProgressRatioAt(
        overlayVisibleForHitTest, view.videoWindow->GetWidth(),
        view.videoWindow->GetHeight(), hitMouse, windowTextCellW,
        windowTextCellH, &progressRatio, &progressUnits);
  }
  if (dragFromThisSurface && leftPressed && !progressHit &&
      !editExitConfirmation) {
    if (hasProgressGeometry) {
      const double unitWidth = std::max(1.0, progressGeometry.unitWidth);
      const double unitHeight = std::max(1.0, progressGeometry.unitHeight);
      const double left = progressGeometry.barX * unitWidth;
      const double width = progressGeometry.barWidth * unitWidth;
      progressGeometry.x =
          std::clamp(progressGeometry.x, left,
                     left + std::max(0.0, width - 1.0));
      progressGeometry.y = progressGeometry.barY * unitHeight;
      progressHit = progressBarRatioAt(progressGeometry, &progressRatio);
    } else if (windowEvent) {
      MouseEvent clampedMouse = hitMouse;
      const int windowWidth = std::max(1, view.videoWindow->GetWidth());
      const int windowHeight = std::max(1, view.videoWindow->GetHeight());
      clampedMouse.pos.X = static_cast<SHORT>(
          std::clamp(static_cast<int>(clampedMouse.pos.X), 0,
                     windowWidth - 1));
      clampedMouse.pos.Y = static_cast<SHORT>(windowHeight - 1);
      progressHit = playback_overlay::windowOverlayProgressRatioAt(
          overlayVisibleForHitTest, windowWidth, windowHeight, clampedMouse,
          windowTextCellW, windowTextCellH, &progressRatio, &progressUnits);
    }
  }
  if (progressHit) {
    triggerOverlay(view, signals);
    *signals.redraw = true;
  }

  if (progressHit && leftPressed && !dragFromThisSurface &&
      hitMouse.eventFlags == 0 && view.videoEdit && view.videoEdit->active &&
      signals.moveVideoEditBoundary) {
    const auto boundary = playback_video_edit::timelineBoundaryAt(
        *view.videoEdit, progressRatio, progressUnits);
    if (boundary) {
      seekState.videoEditBoundaryDrag = boundary;
      seekState.videoEditBoundaryDragFromWindow = windowOriginEvent;
    }
  }
  const bool boundaryDrag =
      seekState.videoEditBoundaryDrag &&
      seekState.videoEditBoundaryDragFromWindow == windowOriginEvent;
  if (progressHit && leftPressed && boundaryDrag) {
    if (const auto targetUs =
            playbackTimelineTargetForRatio(view, progressRatio)) {
      signals.moveVideoEditBoundary(*seekState.videoEditBoundaryDrag,
                                    *targetUs);
    }
    updateOverlayControlHover(signals, -1);
    if (signals.requestTimelinePreview) {
      signals.requestTimelinePreview(previewSurface, progressRatio,
                                     progressUnits);
    }
    queuePlaybackSeekToRatio(view, signals, seekState, progressRatio);
    return;
  }
  const bool seekGesture =
      leftPressed && progressHit &&
      (windowEvent || hitMouse.eventFlags == 0 ||
       hitMouse.eventFlags == MOUSE_MOVED);
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
  if (!overlayVisibleForHitTest) {
    updateOverlayControlHover(signals, -1);
    return;
  }

  playback_overlay::PlaybackOverlayInputs mouseOverlayInputs =
      buildPlaybackMouseOverlayInputs(view, seekState, signals);
  mouseOverlayInputs.osd.controlsVisible = overlayVisibleForHitTest;
  if (textGridHitTestCols > 0 && textGridHitTestRows > 0) {
    mouseOverlayInputs.screenWidth = textGridHitTestCols;
    mouseOverlayInputs.screenHeight = textGridHitTestRows;
    mouseOverlayInputs.progressBarX =
        view.textGridPresentationOutputState->progressBarX;
    mouseOverlayInputs.progressBarY =
        view.textGridPresentationOutputState->progressBarY;
    mouseOverlayInputs.progressBarWidth =
        view.textGridPresentationOutputState->progressBarWidth;
  }
  playback_overlay::PlaybackOverlayState mouseOverlayState =
      playback_overlay::buildPlaybackOverlayState(mouseOverlayInputs);
  const int controlHit =
      windowEvent
          ? playback_overlay::windowOverlayControlAt(mouseOverlayState,
                                                     hitMouse,
                                                     windowTextCellW,
                                                     windowTextCellH)
          : playback_overlay::terminalOverlayControlAt(mouseOverlayState,
                                                       hitMouse);
  updateOverlayControlHover(
      signals, mouseOverlayState.overlayVisible ? controlHit : -1);
  if (controlHit >= 0) {
    triggerOverlay(view, signals);
  }

  if (leftPressed && hitMouse.eventFlags == 0 && controlHit >= 0) {
    if (executeOverlayControl(view, signals, seekState, mouseOverlayState,
                              controlHit)) {
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
                                PlaybackSeekGestureState& seekState) {
  seekState.videoEditBoundaryDrag.reset();
  seekState.videoEditBoundaryDragFromWindow = false;
  if (signals.clearTimelinePreview) {
    signals.clearTimelinePreview(
        playback_video_timeline_preview::PresentationSurface::VideoWindow);
  }
  updateOverlayControlHover(signals, -1);
}

}  // namespace playback_session_input
