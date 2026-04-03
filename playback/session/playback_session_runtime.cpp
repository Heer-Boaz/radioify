#include "videoplayback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "asciiart.h"
#include "asciiart_gpu.h"
#include "audioplayback.h"
#include "gpu_shared.h"
#include "player.h"
#include "playback_dialog.h"
#include "ui_helpers.h"
#include "subtitle_manager.h"
#include "playback_session_state.h"
#include "playback_session_input.h"
#include "playback_window_presenter.h"
#include "playback_screen_renderer.h"
#include "playback_session_bootstrap.h"
#include "videowindow.h"
#include "playback_mode.h"
#include "playback/render/playback_frame_output.h"
#include "playback_session_log.h"

#include "timing_log.h"

static VideoWindow g_videoWindow;
// Centralized GPU frame cache shared between renderers
static GpuVideoFrameCache g_frameCache;
static bool g_windowEnabledPersistent = false;
static bool g_windowEnabledInitialized = false;

namespace {

enum class PlaybackLoopState : uint8_t {
  Running,
  Stopped,
};

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
                              VideoFrame& frameBuffer, VideoFrame*& frame,
                              bool& haveFrame, bool& redraw,
                              bool windowEnabled) {
  bool presented = false;
  VideoFrame nextFrame;
  if (!useWindowPresenter && player.tryGetVideoFrame(&nextFrame)) {
    frameBuffer = std::move(nextFrame);
    haveFrame = true;
    presented = true;
    if (!windowEnabled) {
      redraw = true;
    }
  }
  if (!player.hasVideoFrame()) {
    if (!player.isEnded()) {
      haveFrame = false;
    } else if (frameBuffer.width > 0 && frameBuffer.height > 0) {
      // Keep the last decoded frame visible at EOF.
      haveFrame = true;
    }
  } else if (useWindowPresenter) {
    haveFrame = true;
  }
  frame = &frameBuffer;
  return presented;
}

bool shouldRenderPlaybackFrame(bool redraw, bool overlayVisible,
                               bool debugOverlay,
                               PlaybackSessionState playbackState) {
#if RADIOIFY_ENABLE_TIMING_LOG
  return redraw || ((overlayVisible || debugOverlay) &&
                    playbackState != PlaybackSessionState::Ended);
#else
  (void)debugOverlay;
  return redraw ||
         (overlayVisible && playbackState != PlaybackSessionState::Ended);
#endif
}

}  // namespace

bool showAsciiVideo(const std::filesystem::path& file, ConsoleInput& input,
                    ConsoleScreen& screen, const Style& baseStyle,
                    const Style& accentStyle, const Style& dimStyle,
                    const Style& progressEmptyStyle,
                    const Style& progressFrameStyle,
                    const Color& progressStart, const Color& progressEnd,
                    const VideoPlaybackConfig& config,
                    bool* quitAppRequested) {
  bool quitApplicationRequested = false;
  if (quitAppRequested) {
    *quitAppRequested = false;
  }
  struct QuitFlagScope {
    bool* out = nullptr;
    bool* value = nullptr;
    ~QuitFlagScope() {
      if (out && value) {
        *out = *value;
      }
    }
  } quitFlagScope{quitAppRequested, &quitApplicationRequested};

  bool enableAscii = config.enableAscii;
  bool enableAudio = config.enableAudio && audioIsEnabled();
  // Ensure previous video textures/fences and shared ASCII GPU buffers are not
  // carried into a new session.
  g_frameCache.Reset();
  sharedGpuRenderer().ResetSessionState();

  bool fullRedrawEnabled = enableAscii;
  if (fullRedrawEnabled) {
    screen.setAlwaysFullRedraw(true);
  }

  PerfLog perfLog;
  std::string logError;
  std::filesystem::path logPath =
      std::filesystem::current_path() / "radioify.log";
  configureFfmpegVideoLog(logPath);
  if (!perfLogOpen(&perfLog, logPath, &logError)) {
    playback_dialog::showInfoDialog(input, screen, baseStyle, accentStyle,
                                   dimStyle, "Video error",
                                   "Failed to open timing log file.", logError,
                                   "");
    bool ok = true;
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }
  perfLogAppendf(&perfLog, "video_start file=%s",
                 toUtf8String(file.filename()).c_str());

  auto appendTiming = [&](const std::string& line) {
#if RADIOIFY_ENABLE_TIMING_LOG
    if (line.empty()) return;
    std::lock_guard<std::mutex> lock(timingLogMutex());
    if (perfLog.file.is_open()) {
      perfLog.file << radioifyLogTimestamp() << " " << line << "\n";
    }
#else
    (void)line;
#endif
  };

  auto appendTimingFmt = [&](const char* fmt, ...) {
#if RADIOIFY_ENABLE_TIMING_LOG
    if (!fmt) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int written = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (written <= 0) return;
    if (written >= static_cast<int>(sizeof(buf))) {
      written = static_cast<int>(sizeof(buf)) - 1;
    }
    appendTiming(std::string(buf, buf + written));
#else
    (void)fmt;
#endif
  };

  auto appendVideoError = [&](const std::string& message,
                              const std::string& detail) {
#if RADIOIFY_ENABLE_VIDEO_ERROR_LOG
    std::string line = message.empty() ? "Video error." : message;
    std::string extra = detail;
    if (line.empty() && !extra.empty()) {
      line = extra;
      extra.clear();
    }
    if (line.empty()) {
      line = "Video playback error.";
    }
    std::string payload = "video_error msg=" + line;
    if (!extra.empty()) {
      payload += " detail=" + extra;
    }
    std::ofstream f(logPath, std::ios::app);
    if (f) f << radioifyLogTimestamp() << " " << payload << "\n";
#else
    (void)message;
    (void)detail;
#endif
  };

  auto appendVideoWarning = [&](const std::string& message) {
#if RADIOIFY_ENABLE_VIDEO_ERROR_LOG
    std::string line = message.empty() ? "Video warning." : message;
    std::string payload = "video_warning msg=" + line;
    std::ofstream f(logPath, std::ios::app);
    if (f) f << radioifyLogTimestamp() << " " << payload << "\n";
#else
    (void)message;
#endif
  };

  auto reportVideoError = [&](const std::string& message,
                              const std::string& detail) -> bool {
    appendVideoError(message, detail);
    std::string uiMessage = message.empty() ? "Video playback error." : message;
    std::string uiDetail = detail;
    if (uiMessage.empty() && uiDetail.empty()) {
      uiDetail = "Video playback encountered an unexpected error.";
    }
    playback_dialog::showInfoDialog(input, screen, baseStyle, accentStyle,
                                    dimStyle, "Video error", uiMessage,
                                    uiDetail, "");
    return true;
  };

  Player player;

  PlaybackSessionBootstrapOutcome bootstrapOutcome =
      bootstrapPlaybackSession(file, input, screen, baseStyle, accentStyle,
                               dimStyle, progressEmptyStyle,
                               progressFrameStyle, progressStart, progressEnd,
                               enableAudio, enableAscii, player,
                               &quitApplicationRequested);
  if (bootstrapOutcome != PlaybackSessionBootstrapOutcome::ContinueVideo) {
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return bootstrapOutcome ==
                   PlaybackSessionBootstrapOutcome::PlayAudioOnly
               ? false
               : true;
  }

  SubtitleManager subtitleManager;
  subtitleManager.loadForVideo(file);
  const bool hasSubtitles = subtitleManager.selectableTrackCount() > 0;
  std::atomic<bool> enableSubtitlesShared{hasSubtitles};
  perfLogAppendf(&perfLog, "subtitle_detect tracks=%zu usable=%zu active=%s",
                 subtitleManager.trackCount(),
                 subtitleManager.selectableTrackCount(),
                 subtitleManager.activeTrackLabel().c_str());

  screen.updateSize();
  int initScreenWidth = std::max(20, screen.width());
  int initScreenHeight = std::max(10, screen.height());

  int requestedTargetW = 0;
  int requestedTargetH = 0;
  auto requestTargetSize = [&](int width, int height) -> bool {
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
  };

  if (enableAscii) {
    requestTargetSize(initScreenWidth, initScreenHeight);
  }

  bool audioOk = player.audioOk();
  bool audioStarting = player.audioStarting();
  bool redraw = true;
  bool renderFailed = false;
  std::string renderFailMessage;
  std::string renderFailDetail;
  bool forceRefreshArt = false;
  bool pendingResize = false;
  bool localSeekRequested = false;
  std::atomic<bool> windowLocalSeekRequested{false};
  bool useWindowPresenter = false;
  auto seekRequestTime = std::chrono::steady_clock::time_point::min();
  double pendingSeekTargetSec = -1.0;
  std::atomic<double> windowPendingSeekTargetSec{-1.0};
  auto lastSeekSentTime = std::chrono::steady_clock::time_point::min();
  double queuedSeekTargetSec = -1.0;
  bool seekQueued = false;
  constexpr auto kSeekThrottleInterval = std::chrono::milliseconds(50);
  PlaybackSessionState playbackState = PlaybackSessionState::Active;
  std::atomic<WindowThreadState> windowThreadState{
      WindowThreadState::Disabled};
  std::atomic<bool> windowForcePresent{false};
  std::mutex windowPresentMutex;
  std::condition_variable windowPresentCv;

  AsciiArt art;
  GpuAsciiRenderer& gpuRenderer = sharedGpuRenderer();
  if (!g_windowEnabledInitialized) {
    g_windowEnabledPersistent = config.enableWindow;
    g_windowEnabledInitialized = true;
  }
  bool& windowEnabled = g_windowEnabledPersistent;
  g_videoWindow.SetVsync(true);
  if (g_videoWindow.IsOpen()) {
    g_videoWindow.ShowWindow(windowEnabled);
  }
  if (windowEnabled && !g_videoWindow.IsOpen()) {
    g_videoWindow.Open(1280, 720, "Radioify Output");
    g_videoWindow.ShowWindow(true);
  }
  windowThreadState.store(windowEnabled ? WindowThreadState::Enabled
                                        : WindowThreadState::Disabled,
                          std::memory_order_relaxed);
  if (windowEnabled) {
    windowForcePresent.store(true, std::memory_order_relaxed);
    windowPresentCv.notify_one();
  }
  const bool allowAsciiCpuFallback = false;
  VideoFrame frameBuffer;
  VideoFrame* frame = &frameBuffer;
  bool haveFrame = false;
  int cachedWidth = -1;
  int cachedMaxHeight = -1;
  int cachedFrameWidth = -1;
  int cachedFrameHeight = -1;
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;

  auto finalizeAudioStart = [&]() {
    if (!audioStarting || player.audioStarting()) return;
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
    redraw = true;
  };

  std::atomic<int64_t> overlayUntilMs{0};
  const std::string windowTitle = toUtf8String(file.filename());
  std::atomic<int> overlayControlHover{-1};
  bool loopStopRequested = false;

  playback_session_input::PlaybackInputView inputView;
  inputView.player = &player;
  inputView.screen = &screen;
  inputView.videoWindow = &g_videoWindow;
  inputView.subtitleManager = &subtitleManager;
  inputView.windowTitle = &windowTitle;
  inputView.enableSubtitlesShared = &enableSubtitlesShared;
  inputView.playbackState = &playbackState;
  inputView.audioOk = &audioOk;
  inputView.hasSubtitles = hasSubtitles;
  inputView.progressBarX = &progressBarX;
  inputView.progressBarY = &progressBarY;
  inputView.progressBarWidth = &progressBarWidth;
  inputView.timingSink = appendTiming;

  playback_session_input::PlaybackInputSignals inputSignals;
  inputSignals.overlayControlHover = &overlayControlHover;
  inputSignals.windowThreadState = &windowThreadState;
  inputSignals.windowForcePresent = &windowForcePresent;
  inputSignals.windowPresentCv = &windowPresentCv;
  inputSignals.overlayUntilMs = &overlayUntilMs;
  inputSignals.windowEnabled = &windowEnabled;
  inputSignals.loopStopRequested = &loopStopRequested;
  inputSignals.quitApplicationRequested = &quitApplicationRequested;
  inputSignals.redraw = &redraw;
  inputSignals.forceRefreshArt = &forceRefreshArt;

  playback_session_input::PlaybackSeekState seekState;
  seekState.localSeekRequested = &localSeekRequested;
  seekState.windowLocalSeekRequested = &windowLocalSeekRequested;
  seekState.pendingSeekTargetSec = &pendingSeekTargetSec;
  seekState.windowPendingSeekTargetSec = &windowPendingSeekTargetSec;
  seekState.seekRequestTime = &seekRequestTime;
  seekState.lastSeekSentTime = &lastSeekSentTime;
  seekState.queuedSeekTargetSec = &queuedSeekTargetSec;
  seekState.seekQueued = &seekQueued;

  auto buildWindowUiState = [&]() {
    return playback_window_presenter::buildPlaybackWindowUiState(
        windowTitle, g_videoWindow, player, subtitleManager, playbackState,
        audioOk, hasSubtitles, enableSubtitlesShared,
        windowLocalSeekRequested, windowPendingSeekTargetSec,
        overlayControlHover,
        playback_session_input::isOverlayVisible(inputSignals));
  };
  std::thread windowPresentThread([&]() {
    playback_window_presenter::runWindowPresenterLoop(
        player, g_videoWindow, g_frameCache, windowThreadState,
        windowForcePresent, windowPresentMutex, windowPresentCv,
        [&]() { return playback_session_input::isOverlayVisible(inputSignals); },
        buildWindowUiState);
  });
  auto stopWindowThread = [&]() {
    playback_session_input::signalWindowPresenterStop(inputSignals);
    if (windowPresentThread.joinable()) {
      windowPresentThread.join();
    }
    if (g_videoWindow.IsOpen()) {
      g_videoWindow.Close();
    }
  };
  PlaybackLoopState loopState = PlaybackLoopState::Running;
  auto shutdownPlaybackInfrastructure = [&]() {
    stopWindowThread();
    player.close();
    if (audioOk || audioStarting) audioStop();
    g_frameCache.Reset();
  };

  auto playbackMode = [&]() { return resolvePlaybackMode(enableAscii, windowEnabled); };

  playback_screen_renderer::PlaybackScreenRenderInputs renderInputs;
  renderInputs.screen = &screen;
  renderInputs.videoWindow = &g_videoWindow;
  renderInputs.player = &player;
  renderInputs.subtitleManager = &subtitleManager;
  renderInputs.gpuRenderer = &gpuRenderer;
  renderInputs.frameCache = &g_frameCache;
  renderInputs.art = &art;
  renderInputs.frame = frame;
  renderInputs.windowTitle = &windowTitle;
  renderInputs.baseStyle = &baseStyle;
  renderInputs.accentStyle = &accentStyle;
  renderInputs.dimStyle = &dimStyle;
  renderInputs.progressEmptyStyle = &progressEmptyStyle;
  renderInputs.progressFrameStyle = &progressFrameStyle;
  renderInputs.progressStart = &progressStart;
  renderInputs.progressEnd = &progressEnd;
  renderInputs.enableSubtitlesShared = &enableSubtitlesShared;
  renderInputs.windowLocalSeekRequested = &windowLocalSeekRequested;
  renderInputs.overlayControlHover = &overlayControlHover;
  renderInputs.renderFailed = &renderFailed;
  renderInputs.renderFailMessage = &renderFailMessage;
  renderInputs.renderFailDetail = &renderFailDetail;
  renderInputs.haveFrame = &haveFrame;
  renderInputs.cachedWidth = &cachedWidth;
  renderInputs.cachedMaxHeight = &cachedMaxHeight;
  renderInputs.cachedFrameWidth = &cachedFrameWidth;
  renderInputs.cachedFrameHeight = &cachedFrameHeight;
  renderInputs.progressBarX = &progressBarX;
  renderInputs.progressBarY = &progressBarY;
  renderInputs.progressBarWidth = &progressBarWidth;
  renderInputs.warningSink = appendVideoWarning;
    renderInputs.timingSink = appendTiming;

  auto updateRenderInputs = [&](bool clearHistory, bool frameChanged) {
    renderInputs.debugOverlay = config.debugOverlay;
    renderInputs.currentMode = playbackMode();
    renderInputs.playbackState = playbackState;
    renderInputs.enableAudio = enableAudio;
    renderInputs.audioOk = audioOk;
    renderInputs.audioStarting = audioStarting;
    renderInputs.windowEnabled = windowEnabled;
    renderInputs.hasSubtitles = hasSubtitles;
    renderInputs.allowAsciiCpuFallback = allowAsciiCpuFallback;
    renderInputs.useWindowPresenter = useWindowPresenter;
    renderInputs.overlayVisibleNow =
        playback_session_input::isOverlayVisible(inputSignals);
    renderInputs.clearHistory = clearHistory;
    renderInputs.frameChanged = frameChanged;
    renderInputs.localSeekRequested = localSeekRequested;
    renderInputs.pendingSeekTargetSec = pendingSeekTargetSec;
  };

  useWindowPresenter = windowThreadState.load(std::memory_order_relaxed) ==
                       WindowThreadState::Enabled;
  {
    updateRenderInputs(true, true);
    playback_screen_renderer::renderPlaybackScreen(renderInputs);
  }
  if (renderFailed) {
    shutdownPlaybackInfrastructure();
    bool ok = reportVideoError(renderFailMessage, renderFailDetail);
    {
      updateRenderInputs(true, true);
      playback_screen_renderer::renderPlaybackScreen(renderInputs);
    }
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    sharedGpuRenderer().ResetSessionState();
    return ok;
  }

  auto renderPlaybackFrame = [&](bool presented) {
    auto t0 = std::chrono::steady_clock::now();
    updateRenderInputs(forceRefreshArt, presented);
    playback_screen_renderer::renderPlaybackScreen(renderInputs);
    auto t1 = std::chrono::steady_clock::now();
    auto durMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    if (durMs > 100) {
      appendTimingFmt("video_ui_draw_slow dur_ms=%lld", (long long)durMs);
    }

    if (renderFailed) {
      loopState = PlaybackLoopState::Stopped;
      return;
    }
    redraw = false;
    forceRefreshArt = false;
  };

  while (loopState == PlaybackLoopState::Running) {
    finalizeAudioStart();

    if (g_videoWindow.IsOpen()) {
      g_videoWindow.PollEvents();
      if (windowEnabled && !g_videoWindow.IsVisible()) {
        windowEnabled = false;
        windowThreadState.store(WindowThreadState::Disabled,
                                std::memory_order_relaxed);
        windowPresentCv.notify_one();
        forceRefreshArt = true;
        redraw = true;
      }
    }
    
    if (loopState == PlaybackLoopState::Stopped) break;

    // UI HEARTBEAT
    static auto lastUiHeartbeat = std::chrono::steady_clock::now();
    auto nowUi = std::chrono::steady_clock::now();
    if (nowUi - lastUiHeartbeat >= std::chrono::seconds(1)) {
        bool isPaused = playbackState == PlaybackSessionState::Paused ||
                        audioIsPaused();
        appendTimingFmt("video_heartbeat_ui redraw=%d seeker=%d paused=%d", 
                        redraw ? 1 : 0, localSeekRequested ? 1 : 0,
                        isPaused ? 1 : 0);
        lastUiHeartbeat = nowUi;
    }

    InputEvent ev{};
    auto getNextEvent = [&]() {
        if (input.poll(ev)) return true;
        if (g_videoWindow.IsOpen() && g_videoWindow.PollInput(ev)) {
          return true;
        }
        return false;
    };

    while (getNextEvent()) {
      if (loopState == PlaybackLoopState::Stopped) break;

      if (ev.type == InputEvent::Type::Resize) {
        pendingResize = true;
        redraw = true;
        continue;
      }
      if (ev.type == InputEvent::Type::Key) {
        playback_session_input::handlePlaybackKeyEvent(inputView, inputSignals,
                                                       seekState, ev.key);
        if (loopStopRequested) break;
        continue;
      }
      if (ev.type == InputEvent::Type::Mouse) {
        playback_session_input::handlePlaybackMouseEvent(inputView, inputSignals,
                                                         seekState, ev.mouse);
        if (loopStopRequested) break;
      }
    }
    if (loopStopRequested) {
      loopState = PlaybackLoopState::Stopped;
    }
    if (loopState == PlaybackLoopState::Stopped) break;

    finalizeAudioStart();

    if (windowEnabled && g_videoWindow.IsOpen() && g_videoWindow.IsVisible()) {
      const PlayerState state = player.state();
      const bool isActivelyPlaying =
          state == PlayerState::Playing || state == PlayerState::Draining;
      const bool showCursor =
          playback_session_input::isOverlayVisible(inputSignals) ||
          !isActivelyPlaying;
      g_videoWindow.SetCursorVisible(showCursor);
    } else {
      g_videoWindow.SetCursorVisible(true);
    }

    if (seekQueued) {
      auto now = std::chrono::steady_clock::now();
      bool canSend =
          (lastSeekSentTime == std::chrono::steady_clock::time_point::min()) ||
          (now - lastSeekSentTime >= kSeekThrottleInterval);
      if (canSend) {
        playback_session_input::sendSeekRequest(inputView, inputSignals,
                                                seekState, queuedSeekTargetSec);
      }
    }

    if (pendingResize) {
      screen.updateSize();
      int width = std::max(20, screen.width());
      int height = std::max(10, screen.height());
      if (isAsciiPlaybackMode(playbackMode())) {
        requestTargetSize(width, height);
      }
      pendingResize = false;
      redraw = true;
    }

    useWindowPresenter = windowThreadState.load(std::memory_order_relaxed) ==
                         WindowThreadState::Enabled;
    bool presented = refreshFrameAvailability(player, useWindowPresenter,
                                              frameBuffer, frame, haveFrame,
                                              redraw, windowEnabled);
    syncSeekState(player, localSeekRequested, windowLocalSeekRequested,
                  seekRequestTime, pendingSeekTargetSec,
                  windowPendingSeekTargetSec);
    syncPlaybackEndedState(player, playbackState);

    if (shouldRenderPlaybackFrame(
            redraw, playback_session_input::isOverlayVisible(inputSignals),
            config.debugOverlay, playbackState)) {
      renderPlaybackFrame(presented);
      if (renderFailed) {
        break;
      }
    }

#if RADIOIFY_ENABLE_TIMING_LOG
    if (playbackState == PlaybackSessionState::Ended ||
        (!redraw &&
         !playback_session_input::isOverlayVisible(inputSignals) &&
         !config.debugOverlay)) {
#else
    if (playbackState == PlaybackSessionState::Ended ||
        (!redraw && !playback_session_input::isOverlayVisible(inputSignals))) {
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  if (renderFailed) {
    shutdownPlaybackInfrastructure();
    bool ok = reportVideoError(renderFailMessage, renderFailDetail);
    updateRenderInputs(true, true);
    playback_screen_renderer::renderPlaybackScreen(renderInputs);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    sharedGpuRenderer().ResetSessionState();
    return ok;
  }

  shutdownPlaybackInfrastructure();
  finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
  sharedGpuRenderer().ResetSessionState();
  return true;
}
