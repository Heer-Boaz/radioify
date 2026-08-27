#include "core.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "audioplayback.h"
#include "consolescreen.h"
#include "playback/frame/refresh.h"
#include "playback/video/player.h"
#include "playback/ascii/frame_output.h"
#include "playback/ascii/screen_renderer.h"
#include "input.h"
#include "log.h"

namespace {

bool syncPlaybackEndedState(Player& player,
                            PlaybackSessionState& playbackState) {
  const PlaybackSessionState previous = playbackState;
  if (player.isEnded()) {
    if (playbackState != PlaybackSessionState::Ended) {
      playbackState = PlaybackSessionState::Ended;
    }
    return playbackState != previous;
  }
  if (playbackState == PlaybackSessionState::Ended) {
    playbackState = PlaybackSessionState::Active;
  }
  return playbackState != previous;
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

  void bindRenderInputs(
      playback_screen_renderer::PlaybackScreenRenderInputs& renderInputs) {
    renderInputs.player = &player;
    renderInputs.frame = &frameRefresh.frame;
  }

  void updateRenderInputs(
      playback_screen_renderer::PlaybackScreenRenderInputs& renderInputs) const {
    renderInputs.playbackState = playbackState;
    renderInputs.audioOk = audioOk;
    renderInputs.audioStarting = audioStarting;
    renderInputs.frameAvailable =
        frameRefresh.frameAvailable ||
        (renderInputs.nativeWindowActive && player.hasVideoFrame());
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

  bool applyPresentationSync(bool switchedAwayFromWindow) {
    if (!switchedAwayFromWindow) {
      return false;
    }
    playback_frame_refresh::PlaybackFrameRefreshRequest request;
    request.forceRefresh = true;
    return playback_frame_refresh::refresh(player, frameRefresh, request)
        .frameChanged;
  }

  PlaybackSessionRefreshResult refresh(bool nativeWindowActive,
                                       bool& redraw) {
    playback_frame_refresh::PlaybackFrameRefreshRequest request;
    request.acceptNewFrames = !nativeWindowActive;
    playback_frame_refresh::PlaybackFrameRefreshResult result =
        playback_frame_refresh::refresh(player, frameRefresh, request);
    PlaybackSessionRefreshResult refreshResult;
    refreshResult.framePresented =
        !nativeWindowActive && result.frameChanged;
    if (refreshResult.framePresented) {
      redraw = true;
    }
    refreshResult.stateChanged =
        syncPlaybackEndedState(player, playbackState);
    return refreshResult;
  }

  void setAsciiPresentation(ConsoleScreen& screen, bool enabled) {
    if (enabled) {
      screen.updateSize();
      requestTargetSize(screen.width(), screen.height(),
                        screen.cellPixelWidth(), screen.cellPixelHeight());
      return;
    }

    const int sourceW = player.sourceWidth();
    const int sourceH = player.sourceHeight();
    if (sourceW <= 0 || sourceH <= 0 ||
        (sourceW == requestedTargetW && sourceH == requestedTargetH)) {
      return;
    }
    requestedTargetW = sourceW;
    requestedTargetH = sourceH;
    player.requestResize(sourceW, sourceH);
  }

  void markPendingResize() { pendingResize = true; }

  void handlePendingResize(ConsoleScreen& screen,
                           PlaybackVisualMode visualMode, bool& redraw) {
    if (!pendingResize) {
      return;
    }
    screen.updateSize();
    int width = screen.width();
    int height = screen.height();
    if (visualMode == PlaybackVisualMode::AsciiGrid) {
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

bool PlaybackSessionCore::applyPresentationSync(
    bool switchedAwayFromWindow) {
  return impl_->applyPresentationSync(switchedAwayFromWindow);
}

PlaybackSessionRefreshResult PlaybackSessionCore::refresh(
    bool nativeWindowActive, bool& redraw) {
  return impl_->refresh(nativeWindowActive, redraw);
}

void PlaybackSessionCore::setAsciiPresentation(ConsoleScreen& screen,
                                               bool enabled) {
  impl_->setAsciiPresentation(screen, enabled);
}

uint64_t PlaybackSessionCore::videoFrameCounter() const {
  return impl_->player.videoFrameCounter();
}

bool PlaybackSessionCore::waitForVideoFrame(uint64_t lastCounter,
                                            int timeoutMs) const {
  return impl_->player.waitForVideoFrame(lastCounter, timeoutMs);
}

NativeWaitHandle PlaybackSessionCore::videoFrameWaitHandle() const {
  return impl_->player.videoFrameWaitHandle();
}

void PlaybackSessionCore::markPendingResize() { impl_->markPendingResize(); }

void PlaybackSessionCore::handlePendingResize(ConsoleScreen& screen,
                                              PlaybackVisualMode visualMode,
                                              bool& redraw) {
  impl_->handlePendingResize(screen, visualMode, redraw);
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
