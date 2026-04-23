#include "core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <utility>

#include "audioplayback.h"
#include "consolescreen.h"
#include "playback/frame/refresh.h"
#include "player.h"
#include "playback/ascii/frame_output.h"
#include "playback/ascii/screen_renderer.h"
#include "input.h"
#include "log.h"
#include "presentation.h"

namespace {

void syncSeekState(Player& player, bool& localSeekRequested,
                   std::atomic<bool>& windowLocalSeekRequested,
                   std::chrono::steady_clock::time_point& seekRequestTime,
                   double& pendingSeekTargetSec,
                   std::atomic<double>& windowPendingSeekTargetSec) {
  const bool windowSeekRequested =
      windowLocalSeekRequested.load(std::memory_order_relaxed);
  const bool hasPendingTarget =
      pendingSeekTargetSec >= 0.0 && std::isfinite(pendingSeekTargetSec);
  if (player.isSeeking()) {
    return;
  }

  const PlayerDebugInfo debug = player.debugInfo();
  if (debug.seekInFlightSerial != 0 || debug.pendingSeekSerial != 0) {
    return;
  }

  if (!localSeekRequested && !windowSeekRequested && !hasPendingTarget) {
    pendingSeekTargetSec = -1.0;
    windowPendingSeekTargetSec.store(pendingSeekTargetSec,
                                     std::memory_order_relaxed);
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const bool requestAged =
      seekRequestTime != std::chrono::steady_clock::time_point::min() &&
      now - seekRequestTime > std::chrono::milliseconds(500);
  if (requestAged && player.hasVideoFrame()) {
    localSeekRequested = false;
    windowLocalSeekRequested.store(false, std::memory_order_relaxed);
    pendingSeekTargetSec = -1.0;
    windowPendingSeekTargetSec.store(pendingSeekTargetSec,
                                     std::memory_order_relaxed);
  }
}

void syncPlaybackEndedState(Player& player,
                            PlaybackSessionState& playbackState) {
  if (player.isEnded()) {
    if (playbackState != PlaybackSessionState::Ended) {
      playbackState = PlaybackSessionState::Ended;
    }
    return;
  }
  if (playbackState == PlaybackSessionState::Ended) {
    playbackState = PlaybackSessionState::Active;
  }
}

}  // namespace

struct PlaybackSessionCore::Impl {
  explicit Impl(Args args)
      : player(args.player),
        perfLog(args.perfLog),
        enableAudio(args.enableAudio),
        enableAscii(args.enableAscii),
        audioOk(player.audioOk()),
        audioStarting(player.audioStarting()) {}

  bool requestTargetSize(int width, int height, double cellPixelWidth,
                         double cellPixelHeight) {
    auto [targetW, targetH] =
        playback_frame_output::computeAsciiPlaybackTargetSize(
            width, height, player.sourceWidth(), player.sourceHeight(),
            cellPixelWidth, cellPixelHeight, !player.audioOk());
    if (targetW == requestedTargetW && targetH == requestedTargetH) {
      return false;
    }
    requestedTargetW = targetW;
    requestedTargetH = targetH;
    player.requestResize(targetW, targetH);
    return true;
  }

  void initialize(ConsoleScreen& screen) {
    screen.updateSize();
    if (enableAscii) {
      requestTargetSize(screen.width(),
                        screen.height(), screen.cellPixelWidth(),
                        screen.cellPixelHeight());
    }
  }

  void bindInputView(playback_session_input::PlaybackInputView& inputView) {
    inputView.player = &player;
    inputView.playbackState = &playbackState;
    inputView.audioOk = &audioOk;
  }

  void bindSeekState(playback_session_input::PlaybackSeekState& seekState) {
    seekState.localSeekRequested = &localSeekRequested;
    seekState.windowLocalSeekRequested = &windowLocalSeekRequested;
    seekState.pendingSeekTargetSec = &pendingSeekTargetSec;
    seekState.windowPendingSeekTargetSec = &windowPendingSeekTargetSec;
    seekState.seekRequestTime = &seekRequestTime;
    seekState.lastSeekSentTime = &lastSeekSentTime;
    seekState.queuedSeekTargetSec = &queuedSeekTargetSec;
    seekState.seekQueued = &seekQueued;
  }

  void bindRenderInputs(
      playback_screen_renderer::PlaybackScreenRenderInputs& renderInputs) {
    renderInputs.player = &player;
    renderInputs.frame = &frameRefresh.frame;
    renderInputs.windowLocalSeekRequested = &windowLocalSeekRequested;
  }

  void updateRenderInputs(
      playback_screen_renderer::PlaybackScreenRenderInputs& renderInputs) const {
    renderInputs.playbackState = playbackState;
    renderInputs.audioOk = audioOk;
    renderInputs.audioStarting = audioStarting;
    renderInputs.frameAvailable =
        frameRefresh.frameAvailable ||
        (renderInputs.useWindowPresenter && player.hasVideoFrame());
    renderInputs.localSeekRequested = localSeekRequested;
    renderInputs.pendingSeekTargetSec = pendingSeekTargetSec;
  }

  bool finalizeAudioStart() {
    if (!audioStarting || player.audioStarting()) {
      return false;
    }
    audioOk = player.audioOk();
    audioStarting = false;
    perfLogAppendf(&perfLog, "audio_start ok=%d", audioOk ? 1 : 0);
    if (audioOk) {
      AudioPerfStats stats = audioGetPerfStats();
      if (stats.periodFrames > 0 && stats.periods > 0) {
        perfLogAppendf(
            &perfLog,
            "audio_device period_frames=%u periods=%u buffer_frames=%u rate=%u "
            "channels=%u using_ffmpeg=%d",
            stats.periodFrames, stats.periods, stats.bufferFrames,
            stats.sampleRate, stats.channels, stats.usingFfmpeg ? 1 : 0);
      }
    }
    return true;
  }

  bool applyPresenterSync(const PlaybackPresenterSyncResult& syncResult) {
    if (!syncResult.switchedAwayFromWindow()) {
      return false;
    }
    playback_frame_refresh::PlaybackFrameRefreshRequest request;
    request.forceRefresh = true;
    return playback_frame_refresh::refresh(player, frameRefresh, request)
        .frameChanged;
  }

  bool refresh(bool useWindowPresenter, bool windowActive, bool& redraw) {
    playback_frame_refresh::PlaybackFrameRefreshRequest request;
    request.acceptNewFrames = !useWindowPresenter;
    playback_frame_refresh::PlaybackFrameRefreshResult result =
        playback_frame_refresh::refresh(player, frameRefresh, request);
    bool presented = !useWindowPresenter && result.frameChanged;
    if (presented && !windowActive) {
      redraw = true;
    }
    syncSeekState(player, localSeekRequested, windowLocalSeekRequested,
                  seekRequestTime, pendingSeekTargetSec,
                  windowPendingSeekTargetSec);
    syncPlaybackEndedState(player, playbackState);
    return presented;
  }

  void markPendingResize() { pendingResize = true; }

  void handlePendingResize(ConsoleScreen& screen, PlaybackRenderMode renderMode,
                           bool& redraw) {
    if (!pendingResize) {
      return;
    }
    screen.updateSize();
    int width = screen.width();
    int height = screen.height();
    if (isAsciiPlaybackMode(renderMode)) {
      requestTargetSize(width, height, screen.cellPixelWidth(),
                        screen.cellPixelHeight());
    }
    pendingResize = false;
    redraw = true;
  }

  void shutdownPlayer() {
    if (playerShutdown) {
      return;
    }
    player.close();
    playerShutdown = true;
  }

  void shutdownAudio() {
    if (audioShutdown) {
      return;
    }
    if (audioOk || audioStarting) {
      audioStop();
    }
    audioOk = false;
    audioStarting = false;
    audioShutdown = true;
  }

  void shutdown() {
    shutdownPlayer();
    shutdownAudio();
  }

  Player& player;
  PerfLog& perfLog;
  const bool enableAudio;
  const bool enableAscii;
  bool audioOk;
  bool audioStarting;
  PlaybackSessionState playbackState = PlaybackSessionState::Active;
  playback_frame_refresh::PlaybackFrameRefreshState frameRefresh;
  bool pendingResize = false;
  bool localSeekRequested = false;
  std::atomic<bool> windowLocalSeekRequested{false};
  std::chrono::steady_clock::time_point seekRequestTime =
      std::chrono::steady_clock::time_point::min();
  double pendingSeekTargetSec = -1.0;
  std::atomic<double> windowPendingSeekTargetSec{-1.0};
  std::chrono::steady_clock::time_point lastSeekSentTime =
      std::chrono::steady_clock::time_point::min();
  double queuedSeekTargetSec = -1.0;
  bool seekQueued = false;
  int requestedTargetW = 0;
  int requestedTargetH = 0;
  bool playerShutdown = false;
  bool audioShutdown = false;
};

PlaybackSessionCore::PlaybackSessionCore(Args args)
    : impl_(std::make_unique<Impl>(std::move(args))) {}

PlaybackSessionCore::~PlaybackSessionCore() = default;

PlaybackSessionCore::PlaybackSessionCore(PlaybackSessionCore&&) noexcept =
    default;

PlaybackSessionCore& PlaybackSessionCore::operator=(
    PlaybackSessionCore&&) noexcept = default;

void PlaybackSessionCore::initialize(ConsoleScreen& screen) {
  impl_->initialize(screen);
}

void PlaybackSessionCore::bindInputView(
    playback_session_input::PlaybackInputView& inputView) {
  impl_->bindInputView(inputView);
}

void PlaybackSessionCore::bindSeekState(
    playback_session_input::PlaybackSeekState& seekState) {
  impl_->bindSeekState(seekState);
}

void PlaybackSessionCore::bindRenderInputs(
    playback_screen_renderer::PlaybackScreenRenderInputs& renderInputs) {
  impl_->bindRenderInputs(renderInputs);
}

void PlaybackSessionCore::updateRenderInputs(
    playback_screen_renderer::PlaybackScreenRenderInputs& renderInputs) const {
  impl_->updateRenderInputs(renderInputs);
}

bool PlaybackSessionCore::finalizeAudioStart() {
  return impl_->finalizeAudioStart();
}

bool PlaybackSessionCore::applyPresenterSync(
    const PlaybackPresenterSyncResult& syncResult) {
  return impl_->applyPresenterSync(syncResult);
}

bool PlaybackSessionCore::refresh(bool useWindowPresenter, bool windowActive,
                                  bool& redraw) {
  return impl_->refresh(useWindowPresenter, windowActive, redraw);
}

uint64_t PlaybackSessionCore::videoFrameCounter() const {
  return impl_->player.videoFrameCounter();
}

bool PlaybackSessionCore::waitForVideoFrame(uint64_t lastCounter,
                                            int timeoutMs) const {
  return impl_->player.waitForVideoFrame(lastCounter, timeoutMs);
}

HANDLE PlaybackSessionCore::videoFrameWaitHandle() const {
  return impl_->player.videoFrameWaitHandle();
}

void PlaybackSessionCore::markPendingResize() { impl_->markPendingResize(); }

void PlaybackSessionCore::handlePendingResize(ConsoleScreen& screen,
                                              PlaybackRenderMode renderMode,
                                              bool& redraw) {
  impl_->handlePendingResize(screen, renderMode, redraw);
}

void PlaybackSessionCore::shutdownPlayer() { impl_->shutdownPlayer(); }

void PlaybackSessionCore::shutdownAudio() { impl_->shutdownAudio(); }

void PlaybackSessionCore::shutdown() { impl_->shutdown(); }

Player& PlaybackSessionCore::player() { return impl_->player; }

const Player& PlaybackSessionCore::player() const { return impl_->player; }

PlaybackSessionState PlaybackSessionCore::playbackState() const {
  return impl_->playbackState;
}

bool PlaybackSessionCore::audioOk() const { return impl_->audioOk; }
