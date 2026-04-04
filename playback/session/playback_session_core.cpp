#include "playback_session_core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

#include "audioplayback.h"
#include "consolescreen.h"
#include "player.h"
#include "playback/ascii/playback_frame_output.h"
#include "playback/ascii/playback_screen_renderer.h"
#include "playback_session_input.h"
#include "playback_session_log.h"
#include "playback_session_presentation.h"

namespace {

void syncSeekState(Player& player, bool& localSeekRequested,
                   std::atomic<bool>& windowLocalSeekRequested,
                   std::chrono::steady_clock::time_point& seekRequestTime,
                   double& pendingSeekTargetSec,
                   std::atomic<double>& windowPendingSeekTargetSec) {
  if (localSeekRequested && player.isSeeking()) {
    localSeekRequested = false;
    windowLocalSeekRequested.store(localSeekRequested,
                                   std::memory_order_relaxed);
    return;
  }
  if (localSeekRequested && !player.isSeeking()) {
    auto now = std::chrono::steady_clock::now();
    if (seekRequestTime != std::chrono::steady_clock::time_point::min() &&
        now - seekRequestTime > std::chrono::milliseconds(500) &&
        player.hasVideoFrame()) {
      localSeekRequested = false;
      windowLocalSeekRequested.store(localSeekRequested,
                                     std::memory_order_relaxed);
    }
    return;
  }
  if (!player.isSeeking()) {
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

bool refreshFrameAvailability(Player& player, bool useWindowPresenter,
                              VideoFrame& frameBuffer, bool& haveFrame,
                              bool& redraw, bool windowActive) {
  bool presented = false;
  VideoFrame nextFrame;
  if (!useWindowPresenter && player.tryGetVideoFrame(&nextFrame)) {
    frameBuffer = std::move(nextFrame);
    haveFrame = true;
    presented = true;
    if (!windowActive) {
      redraw = true;
    }
  }
  if (!player.hasVideoFrame()) {
    if (!player.isEnded()) {
      haveFrame = false;
    } else if (frameBuffer.width > 0 && frameBuffer.height > 0) {
      haveFrame = true;
    }
  } else if (useWindowPresenter) {
    haveFrame = true;
  }
  return presented;
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

  bool requestTargetSize(int width, int height) {
    auto [targetW, targetH] =
        playback_frame_output::computeAsciiPlaybackTargetSize(
            width, height, player.sourceWidth(), player.sourceHeight(),
            !player.audioOk());
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
      requestTargetSize(std::max(20, screen.width()),
                        std::max(10, screen.height()));
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
    renderInputs.frame = &frameBuffer;
    renderInputs.windowLocalSeekRequested = &windowLocalSeekRequested;
    renderInputs.haveFrame = &haveFrame;
  }

  void updateRenderInputs(
      playback_screen_renderer::PlaybackScreenRenderInputs& renderInputs) const {
    renderInputs.playbackState = playbackState;
    renderInputs.audioOk = audioOk;
    renderInputs.audioStarting = audioStarting;
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

  void applyPresenterSync(const PlaybackPresenterSyncResult& syncResult) {
    if (syncResult.switchedAwayFromWindow() &&
        player.copyCurrentVideoFrame(&frameBuffer)) {
      haveFrame = true;
    }
  }

  bool refresh(bool useWindowPresenter, bool windowActive, bool& redraw) {
    bool presented = refreshFrameAvailability(player, useWindowPresenter,
                                              frameBuffer, haveFrame, redraw,
                                              windowActive);
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
    int width = std::max(20, screen.width());
    int height = std::max(10, screen.height());
    if (isAsciiPlaybackMode(renderMode)) {
      requestTargetSize(width, height);
    }
    pendingResize = false;
    redraw = true;
  }

  void shutdown() {
    player.close();
    if (audioOk || audioStarting) {
      audioStop();
    }
  }

  Player& player;
  PerfLog& perfLog;
  const bool enableAudio;
  const bool enableAscii;
  bool audioOk;
  bool audioStarting;
  PlaybackSessionState playbackState = PlaybackSessionState::Active;
  VideoFrame frameBuffer;
  bool haveFrame = false;
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

void PlaybackSessionCore::applyPresenterSync(
    const PlaybackPresenterSyncResult& syncResult) {
  impl_->applyPresenterSync(syncResult);
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

void PlaybackSessionCore::shutdown() { impl_->shutdown(); }

Player& PlaybackSessionCore::player() { return impl_->player; }

const Player& PlaybackSessionCore::player() const { return impl_->player; }

PlaybackSessionState PlaybackSessionCore::playbackState() const {
  return impl_->playbackState;
}

bool PlaybackSessionCore::audioOk() const { return impl_->audioOk; }
