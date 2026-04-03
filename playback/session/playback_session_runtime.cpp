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
#include "playback_window_presenter.h"
#include "playback_screen_renderer.h"
#include "playback_overlay.h"
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

void requestWindowRefresh(bool windowEnabled,
                          std::atomic<bool>& windowForcePresent,
                          std::condition_variable& windowPresentCv) {
  if (!windowEnabled) {
    return;
  }
  windowForcePresent.store(true, std::memory_order_relaxed);
  windowPresentCv.notify_one();
}

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

  auto sendSeekRequest = [&](double targetSec) {
    int64_t targetUs =
        static_cast<int64_t>(std::llround(targetSec * 1000000.0));
    player.requestSeek(targetUs);
    pendingSeekTargetSec = targetSec;
    localSeekRequested = true;
    windowPendingSeekTargetSec.store(pendingSeekTargetSec,
                                     std::memory_order_relaxed);
    windowLocalSeekRequested.store(localSeekRequested,
                                   std::memory_order_relaxed);
    seekRequestTime = std::chrono::steady_clock::now();
    lastSeekSentTime = seekRequestTime;
    queuedSeekTargetSec = -1.0;
    seekQueued = false;
    forceRefreshArt = true;
    redraw = true;
    perfLogAppendf(&perfLog,
                   "seek_request target_sec=%.3f target_us=%lld",
                   targetSec, static_cast<long long>(targetUs));
  };

  auto queueSeekRequest = [&](double targetSec) {
    pendingSeekTargetSec = targetSec;
    localSeekRequested = true;
    windowPendingSeekTargetSec.store(pendingSeekTargetSec,
                                     std::memory_order_relaxed);
    windowLocalSeekRequested.store(localSeekRequested,
                                   std::memory_order_relaxed);
    seekRequestTime = std::chrono::steady_clock::now();
    queuedSeekTargetSec = targetSec;
    seekQueued = true;
    forceRefreshArt = true;
    redraw = true;
  };

  constexpr auto kProgressOverlayTimeout = std::chrono::milliseconds(1750);
  constexpr auto kProgressOverlayExtendedTimeout =
      std::chrono::milliseconds(2500);
  auto nowMs = []() -> int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  std::atomic<int64_t> overlayUntilMs{0};
  auto triggerOverlay = [&]() {
    int64_t now = nowMs();
    // If paused/seeking/ended, extend the overlay timeout so user sees controls longer.
    bool extended = false;
    if (playbackState == PlaybackSessionState::Paused) extended = true;
    if (playbackState == PlaybackSessionState::Ended) extended = true;
    if (player.isSeeking()) extended = true;
    int64_t timeoutMs = extended
                            ? static_cast<int64_t>(
                                  kProgressOverlayExtendedTimeout.count())
                            : static_cast<int64_t>(
                                  kProgressOverlayTimeout.count());
    overlayUntilMs.store(now + timeoutMs, std::memory_order_relaxed);
    windowForcePresent.store(true, std::memory_order_relaxed);
    windowPresentCv.notify_one();
  };
  auto overlayVisible = [&]() {
    int64_t until = overlayUntilMs.load(std::memory_order_relaxed);
    if (until <= 0) {
      return false;
    }
    return nowMs() <= until;
  };
  const std::string windowTitle = toUtf8String(file.filename());
  std::atomic<int> overlayControlHover{-1};

  auto toggleWindowEnabled = [&]() {
    windowEnabled = !windowEnabled;
    if (windowEnabled) {
      if (!g_videoWindow.IsOpen()) {
        g_videoWindow.Open(1280, 720, "Radioify Output");
      }
      g_videoWindow.ShowWindow(true);
      windowThreadState.store(WindowThreadState::Enabled,
                              std::memory_order_relaxed);
      windowForcePresent.store(true, std::memory_order_relaxed);
      windowPresentCv.notify_one();
    } else {
      windowThreadState.store(WindowThreadState::Disabled,
                              std::memory_order_relaxed);
      windowPresentCv.notify_one();
      if (g_videoWindow.IsOpen()) {
        g_videoWindow.ShowWindow(false);
      }
      forceRefreshArt = true;
    }
  };

  auto toggleSubtitles = [&]() -> bool {
    if (!hasSubtitles) return false;
    const bool enabled =
        enableSubtitlesShared.load(std::memory_order_relaxed);
    if (!enabled) {
      subtitleManager.selectFirstTrackWithCues();
      enableSubtitlesShared.store(true, std::memory_order_relaxed);
      return true;
    }
    const size_t count = subtitleManager.selectableTrackCount();
    if (count <= 1) {
      enableSubtitlesShared.store(false, std::memory_order_relaxed);
      return true;
    }
    if (subtitleManager.isActiveLastCueTrack()) {
      enableSubtitlesShared.store(false, std::memory_order_relaxed);
      return true;
    }
    return subtitleManager.cycleLanguage();
  };

  auto toggleAudioTrack = [&]() -> bool {
    if (!audioOk) return false;
    return player.cycleAudioTrack();
  };

  auto toggleRadio = [&]() -> bool {
    if (!audioOk) return false;
    audioToggleRadio();
    return true;
  };

  auto toggle50Hz = [&]() -> bool {
    if (!audioOk || !audioSupports50HzToggle()) return false;
    audioToggle50Hz();
    return true;
  };

  auto executeOverlayControl =
      [&](const playback_overlay::PlaybackOverlayState& overlayState,
          int controlIndex) -> bool {
    std::vector<playback_overlay::OverlayControlSpec> specs =
        playback_overlay::buildOverlayControlSpecs(overlayState, -1);
    if (controlIndex < 0 || controlIndex >= static_cast<int>(specs.size())) {
      return false;
    }
    const auto& spec = specs[static_cast<size_t>(controlIndex)];
    switch (spec.id) {
      case playback_overlay::OverlayControlId::Radio:
        return toggleRadio();
      case playback_overlay::OverlayControlId::Hz50:
        return toggle50Hz();
      case playback_overlay::OverlayControlId::AudioTrack:
        return toggleAudioTrack();
      case playback_overlay::OverlayControlId::Subtitles:
        return toggleSubtitles();
    }
    return false;
  };

  auto buildWindowUiState = [&]() {
    return playback_window_presenter::buildPlaybackWindowUiState(
        windowTitle, g_videoWindow, player, subtitleManager, playbackState,
        audioOk, hasSubtitles, enableSubtitlesShared,
        windowLocalSeekRequested, windowPendingSeekTargetSec,
        overlayControlHover, overlayVisible());
  };
  std::thread windowPresentThread([&]() {
    playback_window_presenter::runWindowPresenterLoop(
        player, g_videoWindow, g_frameCache, windowThreadState,
        windowForcePresent, windowPresentMutex, windowPresentCv, overlayVisible,
        buildWindowUiState);
  });
  auto stopWindowThread = [&]() {
    windowThreadState.store(WindowThreadState::Stopping,
                            std::memory_order_relaxed);
    windowForcePresent.store(false, std::memory_order_relaxed);
    windowPresentCv.notify_one();
    if (windowPresentThread.joinable()) {
      windowPresentThread.join();
    }
    if (g_videoWindow.IsOpen()) {
      g_videoWindow.Close();
    }
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
    renderInputs.overlayVisibleNow = overlayVisible();
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
    windowThreadState.store(WindowThreadState::Stopping,
                            std::memory_order_relaxed);
    windowPresentCv.notify_one();
    stopWindowThread();
    player.close();
    if (audioOk || audioStarting) audioStop();
    g_frameCache.Reset();
    bool ok = reportVideoError(renderFailMessage, renderFailDetail);
    {
      updateRenderInputs(true, true);
      playback_screen_renderer::renderPlaybackScreen(renderInputs);
    }
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    sharedGpuRenderer().ResetSessionState();
    return ok;
  }

  PlaybackLoopState loopState = PlaybackLoopState::Running;

  auto requestPlaybackExit = [&](bool quitApp) {
    playbackState = PlaybackSessionState::Exiting;
    loopState = PlaybackLoopState::Stopped;
    g_videoWindow.SetCursorVisible(true);
    windowThreadState.store(WindowThreadState::Stopping,
                            std::memory_order_relaxed);
    windowForcePresent.store(false, std::memory_order_relaxed);
    windowPresentCv.notify_one();
    if (quitApp) {
      quitApplicationRequested = true;
    }
  };

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

  auto buildMouseOverlayInputs = [&]() {
    playback_overlay::PlaybackOverlayInputs mouseOverlayInputs;
    mouseOverlayInputs.windowTitle = windowTitle;
    mouseOverlayInputs.audioOk = audioOk;
    mouseOverlayInputs.audioSupports50HzToggle =
        audioOk && audioSupports50HzToggle();
    mouseOverlayInputs.radioEnabled = audioIsRadioEnabled();
    mouseOverlayInputs.hz50Enabled = audioIs50HzEnabled();
    mouseOverlayInputs.canCycleAudioTracks =
        audioOk && player.canCycleAudioTracks();
    mouseOverlayInputs.activeAudioTrackLabel =
        audioOk ? player.activeAudioTrackLabel() : "N/A";
    mouseOverlayInputs.subtitleManager = &subtitleManager;
    mouseOverlayInputs.hasSubtitles = hasSubtitles;
    mouseOverlayInputs.subtitlesEnabled =
        enableSubtitlesShared.load(std::memory_order_relaxed);
    mouseOverlayInputs.subtitleClockUs = player.currentUs();
    mouseOverlayInputs.seekingOverlay = player.isSeeking() || localSeekRequested;
    mouseOverlayInputs.displaySec =
        std::max(0.0, static_cast<double>(player.currentUs()) / 1000000.0);
    mouseOverlayInputs.totalSec =
        (player.durationUs() > 0)
            ? static_cast<double>(player.durationUs()) / 1000000.0
            : (audioOk ? audioGetTotalSec() : -1.0);
    if (mouseOverlayInputs.totalSec > 0.0) {
      mouseOverlayInputs.displaySec =
          std::clamp(mouseOverlayInputs.displaySec, 0.0,
                     mouseOverlayInputs.totalSec);
    }
    if (mouseOverlayInputs.seekingOverlay &&
        mouseOverlayInputs.totalSec > 0.0 &&
        std::isfinite(mouseOverlayInputs.totalSec)) {
      mouseOverlayInputs.displaySec =
          std::clamp(pendingSeekTargetSec, 0.0, mouseOverlayInputs.totalSec);
    }
    mouseOverlayInputs.volPct =
        static_cast<int>(std::round(audioGetVolume() * 100.0f));
    mouseOverlayInputs.overlayVisible = overlayVisible();
    mouseOverlayInputs.paused =
        playbackState == PlaybackSessionState::Paused ||
        player.state() == PlayerState::Paused;
    mouseOverlayInputs.audioFinished = audioOk && audioIsFinished();
    mouseOverlayInputs.subtitleRenderError =
        g_videoWindow.GetSubtitleRenderError();
    mouseOverlayInputs.screenWidth = std::max(20, screen.width());
    mouseOverlayInputs.screenHeight = std::max(10, screen.height());
    mouseOverlayInputs.windowWidth =
        g_videoWindow.IsOpen() ? g_videoWindow.GetWidth() : 0;
    mouseOverlayInputs.windowHeight =
        g_videoWindow.IsOpen() ? g_videoWindow.GetHeight() : 0;
    mouseOverlayInputs.artTop = 0;
    mouseOverlayInputs.progressBarX = progressBarX;
    mouseOverlayInputs.progressBarY = progressBarY;
    mouseOverlayInputs.progressBarWidth = progressBarWidth;
    return mouseOverlayInputs;
  };

  auto handlePlaybackKeyEvent = [&](const KeyEvent& key) {
    InputCallbacks cb;
    cb.onQuit = [&]() { requestPlaybackExit(true); };
    cb.onTogglePause = [&]() {
      bool pauseNow = playbackState != PlaybackSessionState::Paused;
      if (audioOk) {
        audioTogglePause();
        pauseNow = audioIsPaused();
      } else {
        player.setVideoPaused(pauseNow);
      }
      playbackState = pauseNow ? PlaybackSessionState::Paused
                               : PlaybackSessionState::Active;
    };
    cb.onToggleWindow = [&]() { toggleWindowEnabled(); };
    cb.onToggleRadio = [&]() { toggleRadio(); };
    cb.onToggle50Hz = [&]() { toggle50Hz(); };
    cb.onToggleSubtitles = [&]() { toggleSubtitles(); };
    cb.onToggleAudioTrack = [&]() { toggleAudioTrack(); };
    cb.onSeekBy = [&](int dir) {
      double currentSec = player.currentUs() / 1000000.0;
      sendSeekRequest(currentSec + dir * 5.0);
    };
    cb.onAdjustVolume = [&](float delta) { audioAdjustVolume(delta); };

    if (key.vk == VK_ESCAPE || key.vk == VK_BROWSER_BACK ||
        key.vk == VK_BACK) {
      requestPlaybackExit(false);
      return;
    }

    InputEvent keyEvent{};
    keyEvent.type = InputEvent::Type::Key;
    keyEvent.key = key;
    if (handlePlaybackInput(keyEvent, cb)) {
      triggerOverlay();
      redraw = true;
      requestWindowRefresh(windowEnabled, windowForcePresent, windowPresentCv);
    }
  };

  auto handlePlaybackMouseEvent = [&](const MouseEvent& mouse) {
    int mouseScreenWidth = std::max(20, screen.width());
    int mouseScreenHeight = std::max(10, screen.height());
    const int64_t mouseClockUs = player.currentUs();
    const double mouseDisplaySec =
        mouseClockUs > 0
            ? static_cast<double>(mouseClockUs) / 1000000.0
            : 0.0;
    int64_t mouseDurUs = player.durationUs();
    double mouseTotalSec = -1.0;
    if (mouseDurUs > 0) {
      mouseTotalSec = static_cast<double>(mouseDurUs) / 1000000.0;
    } else if (audioOk) {
      mouseTotalSec = audioGetTotalSec();
    }

    playback_overlay::PlaybackOverlayInputs mouseOverlayInputs =
        buildMouseOverlayInputs();
    if (mouseTotalSec > 0.0) {
      mouseOverlayInputs.displaySec =
          std::clamp(mouseDisplaySec, 0.0, mouseTotalSec);
    }
    if (mouseOverlayInputs.seekingOverlay && mouseTotalSec > 0.0 &&
        std::isfinite(mouseTotalSec)) {
      mouseOverlayInputs.displaySec =
          std::clamp(pendingSeekTargetSec, 0.0, mouseTotalSec);
    }
    mouseOverlayInputs.screenWidth = mouseScreenWidth;
    mouseOverlayInputs.screenHeight = mouseScreenHeight;
    mouseOverlayInputs.windowWidth =
        g_videoWindow.IsOpen() ? g_videoWindow.GetWidth() : 0;
    mouseOverlayInputs.windowHeight =
        g_videoWindow.IsOpen() ? g_videoWindow.GetHeight() : 0;

    playback_overlay::PlaybackOverlayState mouseOverlayState =
        playback_overlay::buildPlaybackOverlayState(mouseOverlayInputs);
    if (playback_overlay::isBackMousePressed(mouse)) {
      requestPlaybackExit(false);
      return;
    }

    bool windowEvent = (mouse.control & 0x80000000) != 0;
    int controlHit =
        windowEvent ? playback_overlay::windowOverlayControlAt(mouseOverlayState, mouse)
                    : playback_overlay::terminalOverlayControlAt(mouseOverlayState, mouse);
    int previousHover = overlayControlHover.load(std::memory_order_relaxed);
    int nextHover = mouseOverlayState.overlayVisible ? controlHit : -1;
    if (nextHover != previousHover) {
      overlayControlHover.store(nextHover, std::memory_order_relaxed);
      redraw = true;
      requestWindowRefresh(windowEnabled, windowForcePresent, windowPresentCv);
    }
    if (controlHit >= 0) {
      triggerOverlay();
    }

    bool progressHit = playback_overlay::isProgressHit(mouseOverlayState, mouse);
    if (progressHit) {
      triggerOverlay();
      redraw = true;
      requestWindowRefresh(windowEnabled, windowForcePresent, windowPresentCv);
    }

    bool leftPressed =
        (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
    if (leftPressed && mouse.eventFlags == 0 && controlHit >= 0) {
      if (executeOverlayControl(mouseOverlayState, controlHit)) {
        triggerOverlay();
        redraw = true;
        requestWindowRefresh(windowEnabled, windowForcePresent, windowPresentCv);
      }
      return;
    }

    if (leftPressed && windowEvent) {
      if (progressHit) {
        float winW = static_cast<float>(g_videoWindow.GetWidth());
        float winH = static_cast<float>(g_videoWindow.GetHeight());
        if (winW > 0.0f && winH > 0.0f) {
          float mouseWinX = static_cast<float>(mouse.pos.X) / winW;
          const float barXLeft = 0.02f;
          const float barXRight = 0.98f;
          double barWidth = static_cast<double>(barXRight - barXLeft);
          double relX = static_cast<double>(mouseWinX - barXLeft);
          double ratio = relX / barWidth;
          ratio = std::clamp(ratio, 0.0, 1.0);
          double totalSec = player.durationUs() / 1000000.0;
          if (totalSec > 0.0 && std::isfinite(totalSec)) {
            double target = ratio * totalSec;
            queueSeekRequest(target);
          }
        }
      }
      return;
    }

    if (leftPressed &&
        (mouse.eventFlags == 0 || mouse.eventFlags == MOUSE_MOVED)) {
      if (progressBarWidth > 0 && mouse.pos.Y == progressBarY &&
          progressBarX >= 0) {
        int rel = mouse.pos.X - progressBarX;
        if (rel >= 0 && rel < progressBarWidth) {
          double denom =
              static_cast<double>(std::max(1, progressBarWidth - 1));
          double ratio = static_cast<double>(rel) / denom;
          ratio = std::clamp(ratio, 0.0, 1.0);
          double totalSec = player.durationUs() / 1000000.0;
          if (totalSec > 0.0 && std::isfinite(totalSec)) {
            double targetSec = ratio * totalSec;
            queueSeekRequest(targetSec);
          }
          return;
        }
      }
    }
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
        handlePlaybackKeyEvent(ev.key);
        continue;
      }
      if (ev.type == InputEvent::Type::Mouse) {
        handlePlaybackMouseEvent(ev.mouse);
      }
    }
    if (loopState == PlaybackLoopState::Stopped) break;

    finalizeAudioStart();

    if (windowEnabled && g_videoWindow.IsOpen() && g_videoWindow.IsVisible()) {
      const PlayerState state = player.state();
      const bool isActivelyPlaying =
          state == PlayerState::Playing || state == PlayerState::Draining;
      const bool showCursor = overlayVisible() || !isActivelyPlaying;
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
        sendSeekRequest(queuedSeekTargetSec);
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

    if (shouldRenderPlaybackFrame(redraw, overlayVisible(), config.debugOverlay,
                                  playbackState)) {
      renderPlaybackFrame(presented);
      if (renderFailed) {
        break;
      }
    }

#if RADIOIFY_ENABLE_TIMING_LOG
    if (playbackState == PlaybackSessionState::Ended ||
        (!redraw && !overlayVisible() && !config.debugOverlay)) {
#else
    if (playbackState == PlaybackSessionState::Ended ||
        (!redraw && !overlayVisible())) {
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  if (renderFailed) {
    windowThreadState.store(WindowThreadState::Stopping,
                            std::memory_order_relaxed);
    windowPresentCv.notify_one();
    stopWindowThread();
    player.close();
    if (audioOk || audioStarting) audioStop();
    g_frameCache.Reset();
    bool ok = reportVideoError(renderFailMessage, renderFailDetail);
    updateRenderInputs(true, true);
    playback_screen_renderer::renderPlaybackScreen(renderInputs);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  stopWindowThread();
  player.close();
  if (audioOk || audioStarting) audioStop();
  g_frameCache.Reset();
  finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
  sharedGpuRenderer().ResetSessionState();
  return true;
}
