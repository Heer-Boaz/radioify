#include "input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "audioplayback.h"
#include "playback/video/player.h"
#include "playback/video/state/machine.h"
#include "playback/input/shortcuts.h"
#include "handoff.h"
#include "playback/video/subtitle/manager.h"
#include "ui_helpers.h"
#include "ui_inputlogic.h"
#include "playback/video/framebuffer/window/window.h"

namespace playback_session_input {

void setPlaybackPaused(const PlaybackInputView& view,
                       PlaybackInputSignals& signals,
                       PlaybackSeekState& seekState, bool paused);

namespace {

bool hasOverlayVisibleWindow(const PlaybackInputSignals& signals) {
  int64_t until = signals.overlayUntilMs->load(std::memory_order_relaxed);
  if (until <= 0) {
    return false;
  }
  int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
  return nowMs <= until;
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

bool readPendingSeekTargetSec(const PlaybackSeekState& seekState,
                              double* outTargetSec) {
  const double targetSec = *seekState.pendingSeekTargetSec;
  if (!(targetSec >= 0.0) || !std::isfinite(targetSec)) {
    return false;
  }
  if (outTargetSec) {
    *outTargetSec = targetSec;
  }
  return true;
}

bool readQueuedSeekTargetSec(const PlaybackSeekState& seekState,
                             double* outTargetSec) {
  if (!*seekState.seekQueued) {
    return false;
  }
  const double targetSec = *seekState.queuedSeekTargetSec;
  if (!(targetSec >= 0.0) || !std::isfinite(targetSec)) {
    return false;
  }
  if (outTargetSec) {
    *outTargetSec = targetSec;
  }
  return true;
}

bool playbackSeekPending(const PlaybackInputView& view,
                         const PlaybackSeekState& seekState) {
  if (readQueuedSeekTargetSec(seekState, nullptr) ||
      readPendingSeekTargetSec(seekState, nullptr)) {
    return true;
  }
  if (*seekState.localSeekRequested) {
    return true;
  }
  if (seekState.windowLocalSeekRequested->load(std::memory_order_relaxed)) {
    return true;
  }
  return view.player->seekPending();
}

bool pauseRequestedByToggle(const PlaybackInputView& view) {
  return playback_session_state::toggleRequestsPause(*view.playbackState,
                                                      view.player->isEnded());
}

double playbackSeekBaseSec(const PlaybackInputView& view,
                           const PlaybackSeekState& seekState) {
  double targetSec = -1.0;
  if (readQueuedSeekTargetSec(seekState, &targetSec) ||
      readPendingSeekTargetSec(seekState, &targetSec)) {
    return clampPlaybackSeekTarget(view, targetSec);
  }
  return clampPlaybackSeekTarget(
      view, static_cast<double>(view.player->currentUs()) / 1000000.0);
}

bool queuePlaybackSeekToRatio(const PlaybackInputView& view,
                              PlaybackInputSignals& signals,
                              PlaybackSeekState& seekState, double ratio) {
  const double totalSec = playbackDurationSec(view);
  if (!(totalSec > 0.0) || !std::isfinite(totalSec)) {
    return false;
  }
  queueSeekRequest(signals, seekState, std::clamp(ratio, 0.0, 1.0) * totalSec);
  return true;
}

void triggerOverlay(const PlaybackInputView& view,
                    const PlaybackInputSignals& signals) {
  int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
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
  int64_t timeoutMs = extended
                          ? static_cast<int64_t>(
                                kProgressOverlayExtendedTimeout.count())
                          : static_cast<int64_t>(
                                kProgressOverlayTimeout.count());
  signals.overlayUntilMs->store(nowMs + timeoutMs, std::memory_order_relaxed);
  requestWindowRefresh(signals);
}

void requestPlaybackExit(const PlaybackInputView& view,
                         PlaybackInputSignals& signals, bool quitApp) {
  *view.playbackState = PlaybackSessionState::Exiting;
  *signals.loopStopRequested = true;
  signals.overlayUntilMs->store(0, std::memory_order_relaxed);
  *signals.redraw = true;
  *signals.forceRefreshArt = true;
  if (quitApp && signals.quitApplicationRequested) {
    *signals.quitApplicationRequested = true;
  }
}

std::chrono::steady_clock::time_point markSeekIntentStarted(
    PlaybackSeekState& seekState) {
  auto now = std::chrono::steady_clock::now();
  *seekState.localSeekRequested = true;
  seekState.windowLocalSeekRequested->store(true, std::memory_order_relaxed);
  return now;
}

void refreshPlaybackInputDisplay(PlaybackInputSignals& signals) {
  *signals.forceRefreshArt = true;
  *signals.redraw = true;
}

void markImmediateSeekActivity(PlaybackInputSignals& signals,
                               PlaybackSeekState& seekState) {
  auto now = markSeekIntentStarted(seekState);
  *seekState.lastSeekSentTime = now;
  *seekState.queuedSeekTargetSec = -1.0;
  *seekState.seekQueued = false;
  refreshPlaybackInputDisplay(signals);
}

void applySeekRequestState(PlaybackInputSignals& signals,
                           PlaybackSeekState& seekState, double targetSec,
                           bool immediate) {
  *seekState.pendingSeekTargetSec = targetSec;
  seekState.windowPendingSeekTargetSec->store(targetSec,
                                              std::memory_order_relaxed);
  if (immediate) {
    markImmediateSeekActivity(signals, seekState);
    return;
  }
  markSeekIntentStarted(seekState);
  *seekState.queuedSeekTargetSec = targetSec;
  *seekState.seekQueued = true;
  refreshPlaybackInputDisplay(signals);
}

void refreshFrameStepRequestDisplay(PlaybackInputSignals& signals) {
  refreshPlaybackInputDisplay(signals);
  requestWindowRefresh(signals);
}

void commitQueuedSeekBeforeFrameStep(const PlaybackInputView& view,
                                     PlaybackInputSignals& signals,
                                     PlaybackSeekState& seekState) {
  double queuedTargetSec = 0.0;
  if (readQueuedSeekTargetSec(seekState, &queuedTargetSec)) {
    sendSeekRequest(view, signals, seekState, queuedTargetSec);
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
                      PlaybackSeekState& seekState,
                      playback_video_frame_step::Direction direction) {
  setPlaybackPaused(view, signals, seekState, true);
  commitQueuedSeekBeforeFrameStep(view, signals, seekState);

  if (!view.player->requestFrameStep(direction)) {
    return false;
  }
  if (*view.playbackState == PlaybackSessionState::Ended) {
    *view.playbackState = PlaybackSessionState::Paused;
  }
  refreshFrameStepRequestDisplay(signals);
  return true;
}

bool executeOverlayControl(const PlaybackInputView& view,
                           PlaybackInputSignals& signals,
                           PlaybackSeekState& seekState,
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
  return playback_overlay::dispatchOverlayControl(spec.id, actions);
}

playback_overlay::PlaybackOverlayInputs buildPlaybackMouseOverlayInputs(
    const PlaybackInputView& view, const PlaybackSeekState& seekState,
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
  const int64_t currentUs = view.player->currentUs();
  inputs.subtitleClockUs = currentUs;
  inputs.seekingOverlay = playbackSeekPending(view, seekState);
  inputs.displaySec = std::max(
      0.0, static_cast<double>(currentUs) / 1000000.0);
  const int64_t durationUs = view.player->durationUs();
  inputs.totalSec = durationUs > 0
                        ? static_cast<double>(durationUs) / 1000000.0
                        : (inputs.audioOk ? audioGetTotalSec() : -1.0);
  if (inputs.totalSec > 0.0) {
    inputs.displaySec = std::clamp(inputs.displaySec, 0.0, inputs.totalSec);
  }
  double pendingSeekTargetSec = 0.0;
  if (inputs.totalSec > 0.0 && std::isfinite(inputs.totalSec) &&
      readPendingSeekTargetSec(seekState, &pendingSeekTargetSec)) {
    inputs.displaySec =
        std::clamp(pendingSeekTargetSec, 0.0, inputs.totalSec);
  }
  inputs.volPct = static_cast<int>(std::round(audioGetVolume() * 100.0f));
  inputs.overlayVisible = isOverlayVisible(signals);
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
  return inputs;
}

}  // namespace

bool isOverlayVisible(const PlaybackInputSignals& signals) {
  return hasOverlayVisibleWindow(signals);
}

void sendSeekRequest(const PlaybackInputView& view,
                     PlaybackInputSignals& signals,
                     PlaybackSeekState& seekState, double targetSec) {
  targetSec = clampPlaybackSeekTarget(view, targetSec);
  int64_t targetUs =
      static_cast<int64_t>(std::llround(targetSec * 1000000.0));
  view.player->requestSeek(targetUs);
  applySeekRequestState(signals, seekState, targetSec, true);
  if (view.timingSink) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "seek_request target_sec=%.3f target_us=%lld",
                  targetSec, static_cast<long long>(targetUs));
    view.timingSink(std::string(buf));
  }
}

void queueSeekRequest(PlaybackInputSignals& signals,
                      PlaybackSeekState& seekState, double targetSec) {
  applySeekRequestState(signals, seekState, targetSec, false);
}

void setPlaybackPaused(const PlaybackInputView& view,
                       PlaybackInputSignals& signals,
                       PlaybackSeekState& seekState, bool paused) {
  const bool ended = *view.playbackState == PlaybackSessionState::Ended ||
                     view.player->isEnded();
  if (!paused && ended) {
    double targetSec = 0.0;
    double pendingTargetSec = 0.0;
    if (readPendingSeekTargetSec(seekState, &pendingTargetSec)) {
      const double durationSec = playbackDurationSec(view);
      if (!(durationSec > 0.0) || pendingTargetSec < durationSec) {
        targetSec = pendingTargetSec;
      }
    }
    sendSeekRequest(view, signals, seekState, targetSec);
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
                              PlaybackSeekState& seekState,
                              const InputEvent& ev) {
  InputCallbacks cb;
  cb.onQuit = [&]() { requestPlaybackExit(view, signals, true); };
  cb.onPlay = [&]() { setPlaybackPaused(view, signals, seekState, false); };
  cb.onPause = [&]() { setPlaybackPaused(view, signals, seekState, true); };
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
      default:
        break;
    }
  };
  cb.onSeekBy = [&](int dir) {
    const double baseSec = playbackSeekBaseSec(view, seekState);
    sendSeekRequest(view, signals, seekState, baseSec + dir * 5.0);
  };
  cb.onPreviousFrame = [&]() {
    requestFrameStep(view, signals, seekState,
                     playback_video_frame_step::Direction::Previous);
  };
  cb.onNextFrame = [&]() {
    requestFrameStep(view, signals, seekState,
                     playback_video_frame_step::Direction::Next);
  };
  cb.onAdjustVolume = [&](float delta) { audioAdjustVolume(delta); };

  const uint32_t shortcutContexts = kPlaybackShortcutContextShared |
                                    kPlaybackShortcutContextGlobal |
                                    kPlaybackShortcutContextPlaybackSession |
                                    kPlaybackShortcutContextVideoPlayback;
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
                                  PlaybackSeekState& seekState,
                                  PlaybackControlCommand command) {
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
                              PlaybackSeekState& seekState,
                              const MouseEvent& mouse) {
  MouseEvent hitMouse = mouse;
  bool overlayVisibleForHitTest = isOverlayVisible(signals);
  const bool windowOriginEvent = isWindowMouseEvent(mouse);
  const bool terminalAsciiProgress =
      !windowOriginEvent && isAsciiPlaybackMode(view.currentMode);
  const playback_frame_output::FrameOutputState* progressOutputState =
      terminalAsciiProgress ? view.frameOutputState : nullptr;
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
      const int localPixelX = pixelX - gridViewport.x;
      const int localPixelY = pixelY - gridViewport.y;
      if (localPixelX >= 0 && localPixelY >= 0 &&
          localPixelX < gridViewport.width &&
          localPixelY < gridViewport.height) {
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
        progressOutputState = view.textGridPresentationOutputState;
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
    requestPlaybackExit(view, signals, false);
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
    ProgressBarHitTestInput hit;
    hit.x = hitMouse.hasPixelPosition ? hitMouse.pixelX : hitMouse.pos.X;
    hit.y = hitMouse.hasPixelPosition ? hitMouse.pixelY : hitMouse.pos.Y;
    hit.barX = progressOutputState->progressBarX;
    hit.barY = progressOutputState->progressBarY;
    hit.barWidth = progressOutputState->progressBarWidth;
    hit.unitWidth = unitWidth;
    hit.unitHeight = unitHeight;
    progressHit = progressBarRatioAt(hit, &progressRatio);
  } else if (windowEvent) {
    progressHit = playback_overlay::windowOverlayProgressRatioAt(
        overlayVisibleForHitTest, view.videoWindow->GetWidth(),
        view.videoWindow->GetHeight(), hitMouse, windowTextCellW,
        windowTextCellH, &progressRatio);
  }
  if (progressHit) {
    triggerOverlay(view, signals);
    *signals.redraw = true;
  }

  const bool leftPressed =
      (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
  const bool seekGesture =
      leftPressed && progressHit &&
      (windowEvent || hitMouse.eventFlags == 0 ||
       hitMouse.eventFlags == MOUSE_MOVED);
  if (progressHit) {
    updateOverlayControlHover(signals, -1);
    if (seekGesture) {
      queuePlaybackSeekToRatio(view, signals, seekState, progressRatio);
    }
    return;
  }
  if (!overlayVisibleForHitTest) {
    updateOverlayControlHover(signals, -1);
    return;
  }

  playback_overlay::PlaybackOverlayInputs mouseOverlayInputs =
      buildPlaybackMouseOverlayInputs(view, seekState, signals);
  mouseOverlayInputs.overlayVisible = overlayVisibleForHitTest;
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

}  // namespace playback_session_input
