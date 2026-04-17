#include "playback_session_input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "audioplayback.h"
#include "player.h"
#include "subtitle_manager.h"
#include "videowindow.h"

namespace playback_session_input {
namespace {

bool hasOverlayVisibleWindow(const PlaybackInputSignals& signals) {
  if (!signals.overlayUntilMs) return false;
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
  if (!signals.desiredLayout || !signals.requestWindowPresent) {
    return;
  }
  if (!isWindowPlaybackLayout(*signals.desiredLayout)) {
    return;
  }
  signals.requestWindowPresent();
}

void triggerOverlay(const PlaybackInputView& view,
                    const PlaybackInputSignals& signals) {
  if (!signals.overlayUntilMs) return;
  int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
  bool extended = false;
  if (view.playbackState &&
      *view.playbackState == PlaybackSessionState::Paused) {
    extended = true;
  }
  if (view.playbackState &&
      *view.playbackState == PlaybackSessionState::Ended) {
    extended = true;
  }
  if (view.player && view.player->isSeeking()) {
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
  if (view.playbackState) {
    *view.playbackState = PlaybackSessionState::Exiting;
  }
  if (signals.loopStopRequested) {
    *signals.loopStopRequested = true;
  }
  if (signals.desiredLayout) {
    *signals.desiredLayout = PlaybackLayout::Terminal;
  }
  if (signals.overlayUntilMs) {
    signals.overlayUntilMs->store(0, std::memory_order_relaxed);
  }
  if (signals.redraw) {
    *signals.redraw = true;
  }
  if (signals.forceRefreshArt) {
    *signals.forceRefreshArt = true;
  }
  if (quitApp && signals.quitApplicationRequested) {
    *signals.quitApplicationRequested = true;
  }
}

void applySeekRequestState(PlaybackInputSignals& signals,
                           PlaybackSeekState& seekState, double targetSec,
                           bool immediate) {
  if (seekState.pendingSeekTargetSec) {
    *seekState.pendingSeekTargetSec = targetSec;
  }
  if (seekState.localSeekRequested) {
    *seekState.localSeekRequested = true;
  }
  if (seekState.windowPendingSeekTargetSec) {
    seekState.windowPendingSeekTargetSec->store(targetSec,
                                                std::memory_order_relaxed);
  }
  if (seekState.windowLocalSeekRequested) {
    seekState.windowLocalSeekRequested->store(true, std::memory_order_relaxed);
  }
  auto now = std::chrono::steady_clock::now();
  if (seekState.seekRequestTime) {
    *seekState.seekRequestTime = now;
  }
  if (immediate) {
    if (seekState.lastSeekSentTime) {
      *seekState.lastSeekSentTime = now;
    }
    if (seekState.queuedSeekTargetSec) {
      *seekState.queuedSeekTargetSec = -1.0;
    }
    if (seekState.seekQueued) {
      *seekState.seekQueued = false;
    }
  } else {
    if (seekState.queuedSeekTargetSec) {
      *seekState.queuedSeekTargetSec = targetSec;
    }
    if (seekState.seekQueued) {
      *seekState.seekQueued = true;
    }
  }
  if (signals.forceRefreshArt) {
    *signals.forceRefreshArt = true;
  }
  if (signals.redraw) {
    *signals.redraw = true;
  }
}

bool toggleRequestedLayout(const PlaybackInputView& view,
                           PlaybackInputSignals& signals) {
  (void)view;
  if (!signals.desiredLayout) {
    return false;
  }
  *signals.desiredLayout = togglePlaybackLayout(*signals.desiredLayout);
  if (signals.redraw) {
    *signals.redraw = true;
  }
  if (signals.forceRefreshArt) {
    *signals.forceRefreshArt = true;
  }
  if (isWindowPlaybackLayout(*signals.desiredLayout)) {
    requestWindowRefresh(signals);
  }
  return true;
}

bool toggleSubtitles(const PlaybackInputView& view) {
  if (!view.hasSubtitles || !view.subtitleManager ||
      !view.enableSubtitlesShared) {
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
  if (!view.player || !view.audioOk || !*view.audioOk) {
    return false;
  }
  return view.player->cycleAudioTrack();
}

bool toggleRadio(const PlaybackInputView& view) {
  if (!view.audioOk || !*view.audioOk) {
    return false;
  }
  audioToggleRadio();
  return true;
}

bool toggle50Hz(const PlaybackInputView& view) {
  if (!view.audioOk || !*view.audioOk || !audioSupports50HzToggle()) {
    return false;
  }
  audioToggle50Hz();
  return true;
}

bool togglePictureInPicture(const PlaybackInputView& view,
                            const PlaybackInputSignals& signals) {
  if (signals.togglePictureInPicture) {
    return signals.togglePictureInPicture();
  }
  if (!view.videoWindow || !view.videoWindow->IsOpen() ||
      !signals.desiredLayout ||
      !isWindowPlaybackLayout(*signals.desiredLayout)) {
    return false;
  }
  return view.videoWindow->TogglePictureInPicture();
}

bool executeOverlayControl(const PlaybackInputView& view,
                           const PlaybackInputSignals& signals,
                           const playback_overlay::PlaybackOverlayState& state,
                           int controlIndex) {
  std::vector<playback_overlay::OverlayControlSpec> specs =
      playback_overlay::buildOverlayControlSpecs(state, -1);
  if (controlIndex < 0 || controlIndex >= static_cast<int>(specs.size())) {
    return false;
  }
  const auto& spec = specs[static_cast<size_t>(controlIndex)];
  switch (spec.id) {
    case playback_overlay::OverlayControlId::Radio:
      return toggleRadio(view);
    case playback_overlay::OverlayControlId::Hz50:
      return toggle50Hz(view);
    case playback_overlay::OverlayControlId::AudioTrack:
      return toggleAudioTrack(view);
    case playback_overlay::OverlayControlId::Subtitles:
      return toggleSubtitles(view);
    case playback_overlay::OverlayControlId::PictureInPicture:
      return togglePictureInPicture(view, signals);
  }
  return false;
}

playback_overlay::PlaybackOverlayInputs buildPlaybackMouseOverlayInputs(
    const PlaybackInputView& view, const PlaybackSeekState& seekState,
    const PlaybackInputSignals& signals) {
  playback_overlay::PlaybackOverlayInputs inputs;
  if (view.windowTitle) {
    inputs.windowTitle = *view.windowTitle;
  }
  inputs.audioOk = view.audioOk && *view.audioOk;
  inputs.audioSupports50HzToggle =
      inputs.audioOk && audioSupports50HzToggle();
  inputs.radioEnabled = audioIsRadioEnabled();
  inputs.hz50Enabled = audioIs50HzEnabled();
  inputs.canCycleAudioTracks =
      inputs.audioOk && view.player && view.player->canCycleAudioTracks();
  inputs.activeAudioTrackLabel =
      inputs.audioOk && view.player ? view.player->activeAudioTrackLabel()
                                    : "N/A";
  inputs.subtitleManager = view.subtitleManager;
  inputs.hasSubtitles = view.hasSubtitles;
  inputs.subtitlesEnabled = view.enableSubtitlesShared &&
                            view.enableSubtitlesShared->load(
                                std::memory_order_relaxed);
  inputs.subtitleClockUs = view.player ? view.player->currentUs() : 0;
  inputs.seekingOverlay =
      (view.player && view.player->isSeeking()) ||
      (seekState.localSeekRequested && *seekState.localSeekRequested);
  inputs.displaySec =
      view.player ? std::max(0.0, static_cast<double>(view.player->currentUs()) /
                                      1000000.0)
                  : 0.0;
  inputs.totalSec =
      (view.player && view.player->durationUs() > 0)
          ? static_cast<double>(view.player->durationUs()) / 1000000.0
          : (inputs.audioOk ? audioGetTotalSec() : -1.0);
  if (inputs.totalSec > 0.0) {
    inputs.displaySec = std::clamp(inputs.displaySec, 0.0, inputs.totalSec);
  }
  if (inputs.seekingOverlay && inputs.totalSec > 0.0 &&
      std::isfinite(inputs.totalSec) && seekState.pendingSeekTargetSec) {
    inputs.displaySec = std::clamp(*seekState.pendingSeekTargetSec, 0.0,
                                   inputs.totalSec);
  }
  inputs.volPct = static_cast<int>(std::round(audioGetVolume() * 100.0f));
  inputs.overlayVisible = isOverlayVisible(signals);
  inputs.paused =
      (view.playbackState &&
       *view.playbackState == PlaybackSessionState::Paused) ||
      (view.player && view.player->state() == PlayerState::Paused);
  inputs.audioFinished = inputs.audioOk && audioIsFinished();
  inputs.pictureInPictureAvailable =
      signals.togglePictureInPicture != nullptr ||
      (view.videoWindow && view.videoWindow->IsOpen());
  inputs.pictureInPictureActive =
      view.videoWindow && view.videoWindow->IsOpen() &&
      view.videoWindow->IsPictureInPicture();
  inputs.subtitleRenderError =
      view.videoWindow ? view.videoWindow->GetSubtitleRenderError() : "";
  inputs.screenWidth =
      view.screen ? std::max(20, view.screen->width()) : 20;
  inputs.screenHeight =
      view.screen ? std::max(10, view.screen->height()) : 10;
  inputs.windowWidth =
      view.videoWindow && view.videoWindow->IsOpen()
          ? view.videoWindow->GetWidth()
          : 0;
  inputs.windowHeight =
      view.videoWindow && view.videoWindow->IsOpen()
          ? view.videoWindow->GetHeight()
          : 0;
  inputs.artTop = 0;
  inputs.progressBarX = view.progressBarX ? *view.progressBarX : -1;
  inputs.progressBarY = view.progressBarY ? *view.progressBarY : -1;
  inputs.progressBarWidth =
      view.progressBarWidth ? *view.progressBarWidth : 0;
  return inputs;
}

}  // namespace

bool isOverlayVisible(const PlaybackInputSignals& signals) {
  return hasOverlayVisibleWindow(signals);
}

void sendSeekRequest(const PlaybackInputView& view,
                     PlaybackInputSignals& signals,
                     PlaybackSeekState& seekState, double targetSec) {
  if (!view.player) return;
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
  if (!view.player || !view.playbackState) return;

  if (!paused && *view.playbackState == PlaybackSessionState::Ended) {
    if (view.audioOk && *view.audioOk && audioIsPaused()) {
      audioTogglePause();
    } else {
      view.player->setVideoPaused(false);
    }
    sendSeekRequest(view, signals, seekState, 0.0);
    *view.playbackState = PlaybackSessionState::Active;
    return;
  }

  if (paused && *view.playbackState == PlaybackSessionState::Ended) {
    return;
  }

  bool pausedNow = paused;
  if (view.audioOk && *view.audioOk) {
    const bool audioPaused = audioIsPaused();
    if (audioPaused != paused) {
      audioTogglePause();
    }
    pausedNow = audioIsPaused();
  } else {
    view.player->setVideoPaused(paused);
  }
  *view.playbackState =
      pausedNow ? PlaybackSessionState::Paused : PlaybackSessionState::Active;
}

bool requestPlaybackTransport(PlaybackInputSignals& signals,
                              PlaybackTransportCommand command) {
  if (!signals.requestTransportCommand) {
    return false;
  }
  return signals.requestTransportCommand(command);
}

void handlePlaybackKeyEvent(const PlaybackInputView& view,
                            PlaybackInputSignals& signals,
                            PlaybackSeekState& seekState, const KeyEvent& key) {
  InputCallbacks cb;
  cb.onQuit = [&]() { requestPlaybackExit(view, signals, true); };
  cb.onPlay = [&]() { setPlaybackPaused(view, signals, seekState, false); };
  cb.onPause = [&]() { setPlaybackPaused(view, signals, seekState, true); };
  cb.onTogglePause = [&]() {
    const bool pauseNow = !view.playbackState ||
                          *view.playbackState != PlaybackSessionState::Paused;
    setPlaybackPaused(view, signals, seekState, pauseNow);
  };
  cb.onStopPlayback = [&]() { requestPlaybackExit(view, signals, false); };
  cb.onPlayPrevious = [&]() {
    if (requestPlaybackTransport(signals, PlaybackTransportCommand::Previous)) {
      requestPlaybackExit(view, signals, false);
    }
  };
  cb.onPlayNext = [&]() {
    if (requestPlaybackTransport(signals, PlaybackTransportCommand::Next)) {
      requestPlaybackExit(view, signals, false);
    }
  };
  cb.onToggleWindow = [&]() { toggleRequestedLayout(view, signals); };
  cb.onToggleRadio = [&]() { toggleRadio(view); };
  cb.onToggle50Hz = [&]() { toggle50Hz(view); };
  cb.onToggleSubtitles = [&]() { toggleSubtitles(view); };
  cb.onToggleAudioTrack = [&]() { toggleAudioTrack(view); };
  cb.onSeekBy = [&](int dir) {
    if (!view.player) return;
    double currentSec = view.player->currentUs() / 1000000.0;
    sendSeekRequest(view, signals, seekState, currentSec + dir * 5.0);
  };
  cb.onAdjustVolume = [&](float delta) { audioAdjustVolume(delta); };

  if (key.vk == VK_ESCAPE || key.vk == VK_BROWSER_BACK || key.vk == VK_BACK) {
    requestPlaybackExit(view, signals, false);
    return;
  }
  if (key.vk == 'P' || key.ch == 'p' || key.ch == 'P') {
    if (togglePictureInPicture(view, signals)) {
      triggerOverlay(view, signals);
      if (signals.redraw) {
        *signals.redraw = true;
      }
      return;
    }
  }
  if (key.vk == 'T' || key.ch == 't' || key.ch == 'T') {
    if (togglePictureInPicture(view, signals)) {
      triggerOverlay(view, signals);
      if (signals.redraw) {
        *signals.redraw = true;
      }
      return;
    }
  }

  InputEvent keyEvent{};
  keyEvent.type = InputEvent::Type::Key;
  keyEvent.key = key;
  if (handlePlaybackInput(keyEvent, cb)) {
    triggerOverlay(view, signals);
    if (signals.redraw) {
      *signals.redraw = true;
    }
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
      const bool pauseNow = !view.playbackState ||
                            *view.playbackState != PlaybackSessionState::Paused;
      setPlaybackPaused(view, signals, seekState, pauseNow);
      break;
    }
    case PlaybackControlCommand::Stop:
      requestPlaybackExit(view, signals, false);
      break;
    case PlaybackControlCommand::Previous:
      if (requestPlaybackTransport(signals, PlaybackTransportCommand::Previous)) {
        requestPlaybackExit(view, signals, false);
      }
      break;
    case PlaybackControlCommand::Next:
      if (requestPlaybackTransport(signals, PlaybackTransportCommand::Next)) {
        requestPlaybackExit(view, signals, false);
      }
      break;
  }
  triggerOverlay(view, signals);
  if (signals.redraw) {
    *signals.redraw = true;
  }
}

void handlePlaybackMouseEvent(const PlaybackInputView& view,
                              PlaybackInputSignals& signals,
                              PlaybackSeekState& seekState,
                              const MouseEvent& mouse) {
  playback_overlay::PlaybackOverlayInputs mouseOverlayInputs =
      buildPlaybackMouseOverlayInputs(view, seekState, signals);
  MouseEvent hitMouse = mouse;
  const bool windowOriginEvent = (mouse.control & 0x80000000) != 0;
  bool windowEvent = windowOriginEvent;
  bool miniGridEvent = false;
  if (windowEvent && view.videoWindow &&
      view.videoWindow->IsPictureInPicture() &&
      view.videoWindow->IsPictureInPictureTextMode()) {
    int gridCols = 0;
    int gridRows = 0;
    view.videoWindow->GetPictureInPictureTextGridSize(gridCols, gridRows);
    const int winW = view.videoWindow->GetWidth();
    const int winH = view.videoWindow->GetHeight();
    int cellW = 1;
    int cellH = 1;
    view.videoWindow->GetPictureInPictureTextCellSize(cellW, cellH);
    if (gridCols > 0 && gridRows > 0 && winW > 0 && winH > 0) {
      const int gridPixelWidth =
          std::min(winW, gridCols * std::max(1, cellW));
      const int gridPixelHeight =
          std::min(winH, gridRows * std::max(1, cellH));
      if (mouse.pos.X >= 0 && mouse.pos.Y >= 0 &&
          mouse.pos.X < gridPixelWidth && mouse.pos.Y < gridPixelHeight) {
        hitMouse.pos.X = static_cast<SHORT>(std::clamp(
            static_cast<int>(mouse.pos.X) / std::max(1, cellW), 0,
            gridCols - 1));
        hitMouse.pos.Y = static_cast<SHORT>(std::clamp(
            static_cast<int>(mouse.pos.Y) / std::max(1, cellH), 0,
            gridRows - 1));
        hitMouse.control &= ~0x80000000;
        mouseOverlayInputs.overlayVisible = true;
        mouseOverlayInputs.screenWidth = gridCols;
        mouseOverlayInputs.screenHeight = gridRows;
        mouseOverlayInputs.progressBarX = 1;
        mouseOverlayInputs.progressBarY = gridRows - 1;
        mouseOverlayInputs.progressBarWidth = std::max(1, gridCols - 2);
        miniGridEvent = true;
      } else {
        hitMouse.control &= ~0x80000000;
        mouseOverlayInputs.overlayVisible = false;
      }
    }
  }
  playback_overlay::PlaybackOverlayState mouseOverlayState =
      playback_overlay::buildPlaybackOverlayState(mouseOverlayInputs);
  if (playback_overlay::isBackMousePressed(mouse)) {
    requestPlaybackExit(view, signals, false);
    return;
  }
  if (windowOriginEvent && mouse.eventFlags == MOUSE_MOVED) {
    triggerOverlay(view, signals);
  }

  windowEvent = (hitMouse.control & 0x80000000) != 0;
  int windowTextCellW = 1;
  int windowTextCellH = 1;
  if (windowEvent && view.videoWindow) {
    view.videoWindow->GetPictureInPictureTextCellSize(windowTextCellW,
                                                      windowTextCellH);
  }
  int controlHit =
      windowEvent
          ? playback_overlay::windowOverlayControlAt(mouseOverlayState,
                                                     hitMouse,
                                                     windowTextCellW,
                                                     windowTextCellH)
          : playback_overlay::terminalOverlayControlAt(mouseOverlayState,
                                                       hitMouse);
  int previousHover = signals.overlayControlHover
                          ? signals.overlayControlHover->load(
                                std::memory_order_relaxed)
                          : -1;
  int nextHover = mouseOverlayState.overlayVisible ? controlHit : -1;
  if (nextHover != previousHover) {
    if (signals.overlayControlHover) {
      signals.overlayControlHover->store(nextHover, std::memory_order_relaxed);
    }
    if (signals.redraw) {
      *signals.redraw = true;
    }
    requestWindowRefresh(signals);
  }
  if (controlHit >= 0) {
    triggerOverlay(view, signals);
  }

  bool progressHit = playback_overlay::isProgressHit(mouseOverlayState, hitMouse);
  if (progressHit) {
    triggerOverlay(view, signals);
    if (signals.redraw) {
      *signals.redraw = true;
    }
  }

  bool leftPressed =
      (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
  if (leftPressed && hitMouse.eventFlags == 0 && controlHit >= 0) {
    if (executeOverlayControl(view, signals, mouseOverlayState, controlHit)) {
      triggerOverlay(view, signals);
      if (signals.redraw) {
        *signals.redraw = true;
      }
    }
    return;
  }

  if (leftPressed && windowEvent) {
    if (progressHit && view.videoWindow) {
      float winW = static_cast<float>(view.videoWindow->GetWidth());
      float winH = static_cast<float>(view.videoWindow->GetHeight());
      if (winW > 0.0f && winH > 0.0f && view.player) {
        float mouseWinX = static_cast<float>(mouse.pos.X) / winW;
        const float barXLeft = 0.02f;
        const float barXRight = 0.98f;
        double barWidth = static_cast<double>(barXRight - barXLeft);
        double relX = static_cast<double>(mouseWinX - barXLeft);
        double ratio = relX / barWidth;
        ratio = std::clamp(ratio, 0.0, 1.0);
        double totalSec = view.player->durationUs() / 1000000.0;
        if (totalSec > 0.0 && std::isfinite(totalSec)) {
          double target = ratio * totalSec;
          queueSeekRequest(signals, seekState, target);
        }
      }
    }
    return;
  }

  if (leftPressed && miniGridEvent) {
    if (progressHit && view.player && mouseOverlayState.progressBarWidth > 0) {
      int rel = hitMouse.pos.X - mouseOverlayState.progressBarX;
      if (rel >= 0 && rel < mouseOverlayState.progressBarWidth) {
        double denom = static_cast<double>(
            std::max(1, mouseOverlayState.progressBarWidth - 1));
        double ratio = static_cast<double>(rel) / denom;
        ratio = std::clamp(ratio, 0.0, 1.0);
        double totalSec = view.player->durationUs() / 1000000.0;
        if (totalSec <= 0.0) {
          totalSec = audioGetTotalSec();
        }
        if (totalSec > 0.0 && std::isfinite(totalSec)) {
          queueSeekRequest(signals, seekState, ratio * totalSec);
        }
      }
    }
    return;
  }

  if (leftPressed && (mouse.eventFlags == 0 || mouse.eventFlags == MOUSE_MOVED)) {
    if (view.progressBarWidth && view.progressBarY && view.progressBarX &&
        *view.progressBarWidth > 0 && mouse.pos.Y == *view.progressBarY &&
        *view.progressBarX >= 0) {
      int rel = mouse.pos.X - *view.progressBarX;
      if (rel >= 0 && rel < *view.progressBarWidth) {
        double denom =
            static_cast<double>(std::max(1, *view.progressBarWidth - 1));
        double ratio = static_cast<double>(rel) / denom;
        ratio = std::clamp(ratio, 0.0, 1.0);
        double totalSec =
            view.player ? view.player->durationUs() / 1000000.0 : -1.0;
        if (totalSec <= 0.0) {
          totalSec = audioGetTotalSec();
        }
        if (totalSec > 0.0 && std::isfinite(totalSec)) {
          double targetSec = ratio * totalSec;
          queueSeekRequest(signals, seekState, targetSec);
        }
        return;
      }
    }
  }
}

}  // namespace playback_session_input
