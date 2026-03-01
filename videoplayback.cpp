#include "videoplayback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>
#include <stdexcept>
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

extern "C" {
#include <libavutil/log.h>
}

#include "asciiart.h"
#include "asciiart_gpu.h"
#include "audioplayback.h"
#include "gpu_shared.h"
#include "player.h"
#include "subtitles.h"
#include "ui_helpers.h"
#include "videowindow.h"

#include "timing_log.h"

static VideoWindow g_videoWindow;
// Centralized GPU frame cache shared between renderers
static GpuVideoFrameCache g_frameCache;
static bool g_windowEnabledPersistent = false;
static bool g_windowEnabledInitialized = false;
static bool g_windowVsyncEnabledPersistent = true;

#if RADIOIFY_ENABLE_TIMING_LOG
static inline std::string now_ms() {
  using namespace std::chrono;
  auto t = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  std::ostringstream ss; ss << t; return ss.str();
}
static inline std::string thread_id_str() {
  std::ostringstream ss; ss << std::this_thread::get_id(); return ss.str();
}
#endif

namespace {
#if RADIOIFY_ENABLE_FFMPEG_ERROR_LOG
std::mutex gFfmpegLogMutex;
std::filesystem::path gFfmpegLogPath;

void ffmpegLogCallback(void* ptr, int level, const char* fmt, va_list vl) {
  if (level > AV_LOG_ERROR) return;
  char line[1024];
  int printPrefix = 1;
  av_log_format_line2(ptr, level, fmt, vl, line, sizeof(line), &printPrefix);
  
  std::lock_guard<std::mutex> lock(timingLogMutex());
  static std::ofstream gFfmpegLogFile;
  static std::filesystem::path gLastPath;

  std::filesystem::path currentPath;
  {
    std::lock_guard<std::mutex> lock2(gFfmpegLogMutex);
    currentPath = gFfmpegLogPath;
  }
  
  if (currentPath.empty()) return;
  if (currentPath != gLastPath) {
    if (gFfmpegLogFile.is_open()) gFfmpegLogFile.close();
    gFfmpegLogFile.open(currentPath, std::ios::app);
    gLastPath = currentPath;
  }
  
  if (!gFfmpegLogFile.is_open()) return;

  // Simple deduplication to prevent flooding during seeking storms
  static std::string lastLine;
  static int repeatCount = 0;
  static auto lastLogTime = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();

  std::string currentLine(line);
  if (currentLine == lastLine && (now - lastLogTime < std::chrono::seconds(1))) {
    repeatCount++;
    if (repeatCount == 100) {
       gFfmpegLogFile << radioifyLogTimestamp() << " ffmpeg_error ... multiple repeats suppressed ...\n";
    }
    return;
  }

  if (repeatCount > 0 && repeatCount != 100) {
     gFfmpegLogFile << radioifyLogTimestamp() << " ffmpeg_error (repeated " << repeatCount << " times)\n";
  }

  lastLine = currentLine;
  repeatCount = 0;
  lastLogTime = now;

  gFfmpegLogFile << radioifyLogTimestamp() << " ffmpeg_error " << line;
  size_t len = std::strlen(line);
  if (len == 0 || line[len - 1] != '\n') {
    gFfmpegLogFile << "\n";
  }
}
#endif

const char* playerStateLabel(PlayerState state) {
  switch (state) {
    case PlayerState::Idle:
      return "Idle";
    case PlayerState::Opening:
      return "Opening";
    case PlayerState::Prefill:
      return "Prefill";
    case PlayerState::Priming:
      return "Priming";
    case PlayerState::Playing:
      return "Playing";
    case PlayerState::Paused:
      return "Paused";
    case PlayerState::Seeking:
      return "Seeking";
    case PlayerState::Draining:
      return "Draining";
    case PlayerState::Ended:
      return "Ended";
    case PlayerState::Error:
      return "Error";
    case PlayerState::Closing:
      return "Closing";
  }
  return "Unknown";
}

const char* clockSourceLabel(PlayerClockSource source) {
  switch (source) {
    case PlayerClockSource::None:
      return "none";
    case PlayerClockSource::Audio:
      return "audio";
    case PlayerClockSource::Video:
      return "video";
  }
  return "none";
}

struct PerfLog {
  std::ofstream file;
  std::string buffer;
  bool enabled = false;
};

bool perfLogOpen(PerfLog* log, const std::filesystem::path& path,
                 std::string* error) {
#if RADIOIFY_ENABLE_TIMING_LOG
  if (!log) return false;
  log->file.open(path, std::ios::out | std::ios::app);
  if (!log->file.is_open()) {
    if (error) {
      *error = "Failed to open timing log file: " + toUtf8String(path);
    }
    return false;
  }
  log->enabled = true;
  return true;
#else
  (void)path;
  (void)error;
  if (log) log->enabled = false;
  return true;
#endif
}

void perfLogFlush(PerfLog* log) {
  if (!log || !log->enabled || log->buffer.empty()) return;
  log->file << log->buffer;
  log->file.flush();
  log->buffer.clear();
}

void perfLogAppendf(PerfLog* log, const char* fmt, ...) {
  if (!log || !log->enabled) return;
  char buf[2048];
  va_list args;
  va_start(args, fmt);
  int written = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (written <= 0) return;
  if (written >= static_cast<int>(sizeof(buf))) {
    written = static_cast<int>(sizeof(buf)) - 1;
  }
  std::string ts = radioifyLogTimestamp();
  log->buffer.append(ts);
  log->buffer.push_back(' ');
  log->buffer.append(buf, buf + written);
  log->buffer.push_back('\n');
  if (log->buffer.size() >= 8192) perfLogFlush(log);
}

void perfLogClose(PerfLog* log) {
  if (!log) return;
  perfLogFlush(log);
  if (log->file.is_open()) {
    log->file.close();
  }
  log->enabled = false;
}

void finalizeVideoPlayback(ConsoleScreen& screen, bool fullRedrawEnabled,
                           PerfLog* log) {
  if (fullRedrawEnabled) {
    screen.setAlwaysFullRedraw(false);
  }
  perfLogClose(log);
}
}  // namespace

void configureFfmpegVideoLog(const std::filesystem::path& path) {
#if RADIOIFY_ENABLE_FFMPEG_ERROR_LOG
  {
    std::lock_guard<std::mutex> lock(gFfmpegLogMutex);
    gFfmpegLogPath = path;
  }
  av_log_set_level(AV_LOG_ERROR);
  av_log_set_callback(ffmpegLogCallback);
#else
  (void)path;
  av_log_set_level(AV_LOG_QUIET);
#endif
}

bool showAsciiVideo(const std::filesystem::path& file, ConsoleInput& input,
                    ConsoleScreen& screen, const Style& baseStyle,
                    const Style& accentStyle, const Style& dimStyle,
                    const Style& progressEmptyStyle,
                    const Style& progressFrameStyle,
                    const Color& progressStart, const Color& progressEnd,
                    const VideoPlaybackConfig& config) {
  bool enableAscii = config.enableAscii;
  bool enableAudio = config.enableAudio && audioIsEnabled();
  bool enableSubtitles = config.enableSubtitles;
  // Ensure previous video textures/fences are not carried into a new session.
  g_frameCache.Reset();

  bool fullRedrawEnabled = enableAscii;
  if (fullRedrawEnabled) {
    screen.setAlwaysFullRedraw(true);
  }

  auto renderDialog = [&](const std::string& title,
                          const std::string& message,
                          const std::string& detail,
                          const std::string& footer) {
    screen.updateSize();
    int width = std::max(20, screen.width());
    int height = std::max(10, screen.height());
    screen.clear(baseStyle);
    int y = 0;
    if (!title.empty()) {
      screen.writeText(0, y++, fitLine(title, width), accentStyle);
    }
    if (!message.empty() && y < height) {
      screen.writeText(0, y++, fitLine(message, width), baseStyle);
    }
    if (!detail.empty() && y < height) {
      screen.writeText(0, y++, fitLine(detail, width), dimStyle);
    }
    if (!footer.empty()) {
      screen.writeText(0, height - 1, fitLine(footer, width), dimStyle);
    }
    screen.draw();
  };

  auto showError = [&](const std::string& message,
                       const std::string& detail) -> bool {
    renderDialog("Video error", message, detail, "Press Enter to continue.");
    InputEvent ev{};
    while (true) {
      while (input.poll(ev)) {
        if (ev.type == InputEvent::Type::Key) {
          const KeyEvent& key = ev.key;
          if (key.vk == VK_RETURN || key.vk == VK_ESCAPE ||
              key.vk == VK_BROWSER_BACK || key.vk == VK_BACK) {
            return true;
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  };

  auto showAudioFallbackPrompt = [&](const std::string& message,
                                     const std::string& detail) -> bool {
    renderDialog("Audio only?", message, detail,
                 "Enter: play audio  Esc: cancel");
    InputEvent ev{};
    while (true) {
      while (input.poll(ev)) {
        if (ev.type == InputEvent::Type::Key) {
          const KeyEvent& key = ev.key;
          if (key.vk == VK_RETURN) return true;
          if (key.vk == VK_ESCAPE || key.vk == VK_BROWSER_BACK ||
              key.vk == VK_BACK) {
            return false;
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  };

  PerfLog perfLog;
  std::string logError;
  std::filesystem::path logPath =
      std::filesystem::current_path() / "radioify.log";
  configureFfmpegVideoLog(logPath);
  if (!perfLogOpen(&perfLog, logPath, &logError)) {
    bool ok = showError("Failed to open timing log file.", logError);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }
  perfLogAppendf(&perfLog, "video_start file=%s",
                 toUtf8String(file.filename()).c_str());

  auto appendTiming = [&](const std::string& s) {
#if RADIOIFY_ENABLE_TIMING_LOG
    std::lock_guard<std::mutex> lock(timingLogMutex());
    if (perfLog.file.is_open()) {
      perfLog.file << radioifyLogTimestamp() << " " << s << "\n";
    }
#else
    (void)s;
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
    std::lock_guard<std::mutex> lock(timingLogMutex());
    if (perfLog.file.is_open()) {
      perfLog.file << radioifyLogTimestamp() << " " << std::string(buf, buf + written)
                   << "\n";
    }
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
    return true;
  };

  Player player;
  PlayerConfig playerConfig;
  playerConfig.file = file;
  playerConfig.logPath = logPath;
  playerConfig.enableAudio = enableAudio;
  playerConfig.allowDecoderScale = enableAscii;
  SubtitleTrack subtitles;
  std::filesystem::path subtitlePath;
  auto findSubtitlePath =
      [&](const std::filesystem::path& videoPath) -> std::filesystem::path {
    std::error_code ec;
    std::filesystem::path baseDir = videoPath.parent_path();
    std::string stem = videoPath.stem().string();
    const char* exts[] = {".srt", ".vtt"};
    for (const char* ext : exts) {
      std::filesystem::path candidate = baseDir / (stem + ext);
      if (std::filesystem::exists(candidate, ec) && !ec) {
        return candidate;
      }
    }
    return {};
  };

  if (!player.open(playerConfig, nullptr)) {
    bool ok = showError("Failed to open video.", "");
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  constexpr auto kPrepRedrawInterval = std::chrono::milliseconds(120);
  constexpr double kPrepPulseSeconds = 1.6;
  auto renderPreparingScreen = [&](double progress) {
    screen.updateSize();
    int width = std::max(20, screen.width());
    int height = std::max(10, screen.height());
    screen.clear(baseStyle);
    std::string title = "Video: " + toUtf8String(file.filename());
    screen.writeText(0, 0, fitLine(title, width), accentStyle);
    std::string message = "Preparing video playback...";
    int msgLine = std::clamp(height / 2, 1, std::max(1, height - 2));
    int msgWidth = utf8CodepointCount(message);
    if (msgWidth >= width) {
      screen.writeText(0, msgLine, fitLine(message, width), dimStyle);
    } else {
      int msgX = (width - msgWidth) / 2;
      screen.writeText(msgX, msgLine, message, dimStyle);
    }
    int barWidth = std::min(32, width - 6);
    int barLine = msgLine + 1;
    if (barWidth >= 5 && barLine < height) {
      int barX = std::max(0, (width - (barWidth + 2)) / 2);
      screen.writeChar(barX, barLine, L'|', progressFrameStyle);
      auto barCells = renderProgressBarCells(progress, barWidth,
                                             progressEmptyStyle, progressStart,
                                             progressEnd);
      for (int i = 0; i < barWidth; ++i) {
        const auto& cell = barCells[static_cast<size_t>(i)];
        screen.writeChar(barX + 1 + i, barLine, cell.ch, cell.style);
      }
      screen.writeChar(barX + 1 + barWidth, barLine, L'|',
                       progressFrameStyle);
    }
    screen.draw();
  };

  bool running = true;
  auto initStart = std::chrono::steady_clock::now();
  auto lastInitDraw = std::chrono::steady_clock::time_point::min();
  while (running) {
    if (player.initDone()) {
      break;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - lastInitDraw >= kPrepRedrawInterval) {
      double elapsed = std::chrono::duration<double>(now - initStart).count();
      double phase = std::fmod(elapsed, kPrepPulseSeconds);
      double ratio = (phase <= (kPrepPulseSeconds * 0.5))
                         ? (phase / (kPrepPulseSeconds * 0.5))
                         : ((kPrepPulseSeconds - phase) /
                            (kPrepPulseSeconds * 0.5));
      renderPreparingScreen(ratio);
      lastInitDraw = now;
    }
    InputEvent ev{};
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Key) {
        const KeyEvent& key = ev.key;
        const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
        bool ctrl = (key.control & ctrlMask) != 0;
        if ((key.vk == 'C' || key.ch == 'c' || key.ch == 'C') && ctrl) {
          running = false;
          break;
        }
        if (key.vk == VK_ESCAPE || key.vk == VK_BROWSER_BACK ||
            key.vk == VK_BACK) {
          running = false;
          break;
        }
      } else if (ev.type == InputEvent::Type::Mouse) {
        const MouseEvent& mouse = ev.mouse;
        const DWORD backMask = RIGHTMOST_BUTTON_PRESSED |
                               FROM_LEFT_2ND_BUTTON_PRESSED |
                               FROM_LEFT_3RD_BUTTON_PRESSED |
                               FROM_LEFT_4TH_BUTTON_PRESSED;
        if ((mouse.buttonState & backMask) != 0) {
          running = false;
          break;
        }
      } else if (ev.type == InputEvent::Type::Resize) {
        lastInitDraw = std::chrono::steady_clock::time_point::min();
      }
    }
  }

  if (!running) {
    player.close();
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return true;
  }
  if (!player.initOk()) {
    player.close();
    std::string initError = player.initError();
    if (initError.rfind("No video stream found", 0) == 0) {
      if (!enableAudio) {
        bool ok =
            showError("No video stream found.", "Audio playback is disabled.");
        finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
        return ok;
      }
      bool playAudio = showAudioFallbackPrompt(
          "No video stream found.", "This file can be played as audio only.");
      finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
      return playAudio ? false : true;
    }
    if (initError.empty()) {
      initError = "Failed to open video.";
    }
    bool ok = showError(initError, "");
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  if (enableSubtitles) {
    subtitlePath = findSubtitlePath(file);
    if (!subtitlePath.empty()) {
      std::string subtitleError;
      if (subtitles.loadFromFile(subtitlePath, &subtitleError)) {
        perfLogAppendf(&perfLog, "subtitle_load ok=1 file=%s cues=%zu",
                       toUtf8String(subtitlePath.filename()).c_str(),
                       subtitles.size());
      } else {
        perfLogAppendf(&perfLog, "subtitle_load ok=0 file=%s err=%s",
                       toUtf8String(subtitlePath.filename()).c_str(),
                       subtitleError.c_str());
      }
    }
  }

  int sourceWidth = player.sourceWidth();
  int sourceHeight = player.sourceHeight();

  screen.updateSize();
  int initScreenWidth = std::max(20, screen.width());
  int initScreenHeight = std::max(10, screen.height());

  auto computeTargetSizeForSource = [&](int width, int height, int srcW,
                                        int srcH, bool showStatusLine) {
    int headerLines = showStatusLine ? 1 : 0;
    const int footerLines = 0;
    int maxHeight = std::max(1, height - headerLines - footerLines);
    int maxOutW = std::max(1, width - 8);
    AsciiArtLayout fitted =
        fitAsciiArtLayout(srcW, srcH, maxOutW, std::max(1, maxHeight));
    int outW = fitted.width;
    int outH = fitted.height;
    int targetW = std::max(2, outW * 2);
    int targetH = std::max(4, outH * 4);
    if (targetW & 1) ++targetW;
    if (targetH & 1) ++targetH;
    if (srcW > 0) targetW = std::min(targetW, srcW & ~1);
    if (srcH > 0) targetH = std::min(targetH, srcH & ~1);
    targetW = std::max(2, targetW);
    targetH = std::max(4, targetH);

    const int kMaxDecodeWidth = 1024;
    const int kMaxDecodeHeight = 768;
    if (targetW > kMaxDecodeWidth || targetH > kMaxDecodeHeight) {
      double scaleW = static_cast<double>(kMaxDecodeWidth) / targetW;
      double scaleH = static_cast<double>(kMaxDecodeHeight) / targetH;
      double scale = std::min(scaleW, scaleH);
      targetW = static_cast<int>(std::lround(targetW * scale));
      targetH = static_cast<int>(std::lround(targetH * scale));
      targetW = std::min(targetW, kMaxDecodeWidth);
      targetH = std::min(targetH, kMaxDecodeHeight);
      targetW &= ~1;
      targetH &= ~1;
      targetW = std::max(2, targetW);
      targetH = std::max(4, targetH);
    }
    return std::pair<int, int>(targetW, targetH);
  };

  auto computeTargetSize = [&](int width, int height) {
    bool showStatusLine = !player.audioOk();
    return computeTargetSizeForSource(width, height, sourceWidth, sourceHeight,
                                      showStatusLine);
  };

  auto computeAsciiOutputSize = [&](int maxWidth, int maxHeight, int srcW,
                                    int srcH) {
    int maxOutW = std::max(1, maxWidth - 8);
    AsciiArtLayout fitted =
        fitAsciiArtLayout(srcW, srcH, maxOutW, std::max(1, maxHeight));
    return std::pair<int, int>(fitted.width, fitted.height);
  };

  int requestedTargetW = 0;
  int requestedTargetH = 0;
  auto requestTargetSize = [&](int width, int height) -> bool {
    auto [targetW, targetH] = computeTargetSize(width, height);
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
  bool userPaused = false;
  bool localSeekRequested = false;
  std::atomic<bool> windowLocalSeekRequested{false};
  bool closeWindowRequested = false;
  bool useWindowPresenter = false;
  auto seekRequestTime = std::chrono::steady_clock::time_point::min();
  double pendingSeekTargetSec = -1.0;
  std::atomic<double> windowPendingSeekTargetSec{-1.0};
  auto lastSeekSentTime = std::chrono::steady_clock::time_point::min();
  double queuedSeekTargetSec = -1.0;
  bool seekQueued = false;
  constexpr auto kSeekThrottleInterval = std::chrono::milliseconds(50);
  bool ended = false;
  auto lastUiDbgLog = std::chrono::steady_clock::time_point::min();
  std::string lastUiDbgLine1;
  std::string lastUiDbgLine2;
  std::atomic<bool> windowThreadRunning{true};
  std::atomic<bool> windowThreadEnabled{false};
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
  bool& windowVsyncEnabled = g_windowVsyncEnabledPersistent;
  g_videoWindow.SetVsync(windowVsyncEnabled);
  if (g_videoWindow.IsOpen()) {
    g_videoWindow.ShowWindow(windowEnabled);
  }
  if (windowEnabled && !g_videoWindow.IsOpen()) {
    g_videoWindow.Open(1280, 720, "Radioify Output");
    g_videoWindow.ShowWindow(true);
  }
  windowThreadEnabled.store(windowEnabled, std::memory_order_relaxed);
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
    // If paused/seeking/ended, extend the overlay timeout so user sees controls longer
    bool extended = false;
    if (userPaused) extended = true;
    if (player.isSeeking()) extended = true;
    // 'ended' may not be set yet at first calls; it's captured by reference and can be used
    // if present later
    // Use a longer duration for extended cases
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
  auto isBackMousePressed = [&](const MouseEvent& mouse) {
    const DWORD backMask = RIGHTMOST_BUTTON_PRESSED |
                           FROM_LEFT_2ND_BUTTON_PRESSED |
                           FROM_LEFT_3RD_BUTTON_PRESSED |
                           FROM_LEFT_4TH_BUTTON_PRESSED;
    return (mouse.buttonState & backMask) != 0;
  };
  auto isWindowProgressHit = [&](const MouseEvent& mouse) {
    float winW = static_cast<float>(g_videoWindow.GetWidth());
    float winH = static_cast<float>(g_videoWindow.GetHeight());
    if (winW <= 0.0f || winH <= 0.0f) {
      return false;
    }
    float mouseWinX = static_cast<float>(mouse.pos.X) / winW;
    float mouseWinY = static_cast<float>(mouse.pos.Y) / winH;
    const float barYTop = 0.95f;
    const float barYBot = 1.0f;
    const float barXLeft = 0.02f;
    const float barXRight = 0.98f;
    return mouseWinX >= barXLeft && mouseWinX <= barXRight &&
           mouseWinY >= barYTop && mouseWinY <= barYBot;
  };
  auto isTerminalProgressHit = [&](const MouseEvent& mouse) {
    int screenHeight = std::max(10, screen.height());
    int barY = progressBarY >= 0 ? progressBarY : (screenHeight - 1);
    if (mouse.pos.Y != barY) {
      return false;
    }
    if (progressBarWidth > 0 && progressBarX >= 0) {
      return mouse.pos.X >= progressBarX &&
             mouse.pos.X < progressBarX + progressBarWidth;
    }
    return mouse.pos.X >= 0;
  };
  auto isProgressHit = [&](const MouseEvent& mouse) {
    bool windowEvent = (mouse.control & 0x80000000) != 0;
    return windowEvent ? isWindowProgressHit(mouse)
                       : isTerminalProgressHit(mouse);
  };

  auto getSubtitleText = [&](int64_t clockUs, bool seeking) -> std::string {
    if (!enableSubtitles || seeking || clockUs <= 0 || subtitles.empty()) {
      return {};
    }
    const SubtitleCue* cue = subtitles.cueAt(clockUs);
    if (!cue) return {};
    return cue->text;
  };

  const std::string windowTitle = toUtf8String(file.filename());
  auto buildWindowUiState = [&]() {
    WindowUiState ui;
    double currentSec = 0.0;
    double totalSec = -1.0;
    int64_t clockUs = player.currentUs();
    if (clockUs > 0) {
      currentSec = static_cast<double>(clockUs) / 1000000.0;
    }
    int64_t durUs = player.durationUs();
    if (durUs > 0) {
      totalSec = static_cast<double>(durUs) / 1000000.0;
    } else if (player.audioOk()) {
      totalSec = audioGetTotalSec();
    }
    if (totalSec > 0.0) {
      currentSec = std::clamp(currentSec, 0.0, totalSec);
    }
    double displaySec = currentSec;
    bool seekingOverlay =
        player.isSeeking() ||
        windowLocalSeekRequested.load(std::memory_order_relaxed);
    double pendingTarget =
        windowPendingSeekTargetSec.load(std::memory_order_relaxed);
    if (seekingOverlay && pendingTarget >= 0.0 && totalSec > 0.0 &&
        std::isfinite(totalSec)) {
      displaySec = std::clamp(pendingTarget, 0.0, totalSec);
    }

    double ratio = 0.0;
    if (totalSec > 0.0 && std::isfinite(totalSec)) {
      ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
    }

    ui.progress = static_cast<float>(ratio);
    ui.isPaused = player.state() == PlayerState::Paused;
    ui.overlayAlpha = overlayVisible() ? 1.0f : 0.0f;
    ui.title = windowTitle;
    ui.displaySec = displaySec;
    ui.totalSec = totalSec;
    ui.volPct = static_cast<int>(std::round(audioGetVolume() * 100.0f));
    ui.vsyncEnabled = g_videoWindow.IsVsyncEnabled();
    std::string subtitle = getSubtitleText(clockUs, seekingOverlay);
    ui.subtitle = subtitle;
    ui.subtitleAlpha = subtitle.empty() ? 0.0f : 1.0f;
    return ui;
  };

  std::thread windowPresentThread([&]() {
    VideoFrame localFrame;
    uint64_t lastCounter = player.videoFrameCounter();
    while (windowThreadRunning.load(std::memory_order_relaxed)) {
      if (!windowThreadEnabled.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lock(windowPresentMutex);
        windowPresentCv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
          return !windowThreadRunning.load(std::memory_order_relaxed) ||
                 windowThreadEnabled.load(std::memory_order_relaxed);
        });
        continue;
      }

      if (!g_videoWindow.IsOpen() || !g_videoWindow.IsVisible()) {
        std::unique_lock<std::mutex> lock(windowPresentMutex);
        windowPresentCv.wait_for(lock, std::chrono::milliseconds(50));
        continue;
      }

      bool waitedForFrame = false;
      bool shouldWaitForFrame =
          !windowForcePresent.load(std::memory_order_relaxed) &&
          !overlayVisible() && !player.isSeeking();
      if (shouldWaitForFrame) {
        waitedForFrame = true;
        player.waitForVideoFrame(lastCounter, 16);
        if (!windowThreadRunning.load(std::memory_order_relaxed)) {
          break;
        }
      }

      uint64_t counterNow = player.videoFrameCounter();
      bool frameChanged = false;
      if (counterNow != lastCounter) {
        frameChanged = player.tryGetVideoFrame(&localFrame);
        lastCounter = counterNow;
      }
      if (!windowThreadRunning.load(std::memory_order_relaxed)) {
        break;
      }
      if (frameChanged) {
        if (localFrame.format != VideoPixelFormat::HWTexture ||
            !localFrame.hwTexture) {
          frameChanged = false;
        } else {
          D3D11_TEXTURE2D_DESC desc{};
          localFrame.hwTexture->GetDesc(&desc);
          bool is10Bit = (desc.Format == DXGI_FORMAT_P010);

          ID3D11Device* device = getSharedGpuDevice();
          if (device) {
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            device->GetImmediateContext(&context);
            if (context) {
              std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
              bool updated = g_frameCache.Update(
                  device, context.Get(), localFrame.hwTexture.Get(),
                  localFrame.hwTextureArrayIndex, localFrame.width,
                  localFrame.height, localFrame.fullRange, localFrame.yuvMatrix,
                  localFrame.yuvTransfer, is10Bit ? 10 : 8,
                  localFrame.rotationQuarterTurns);
              if (!updated) {
                frameChanged = false;
              }
            }
          }
        }
      }

      WindowUiState ui = buildWindowUiState();
      bool overlayVisibleNow = ui.overlayAlpha > 0.01f;
      bool forcePresent =
          windowForcePresent.exchange(false, std::memory_order_relaxed);
      bool needsPresent =
          frameChanged || forcePresent || overlayVisibleNow || player.isSeeking();

      if (needsPresent) {
        if (!windowThreadRunning.load(std::memory_order_relaxed)) {
          break;
        }
        g_frameCache.WaitForFrameLatency(
            16, g_videoWindow.GetFrameLatencyWaitableObject());
        if (!windowThreadRunning.load(std::memory_order_relaxed)) {
          break;
        }
        if (frameChanged) {
          g_videoWindow.Present(g_frameCache, ui, true);
        } else {
          g_videoWindow.PresentOverlay(g_frameCache, ui, true);
        }
      } else if (!waitedForFrame) {
        std::unique_lock<std::mutex> lock(windowPresentMutex);
        windowPresentCv.wait_for(lock, std::chrono::milliseconds(10));
      }
    }
  });
  auto stopWindowThread = [&]() {
    windowThreadEnabled.store(false, std::memory_order_relaxed);
    windowForcePresent.store(false, std::memory_order_relaxed);
    windowThreadRunning.store(false, std::memory_order_relaxed);
    windowPresentCv.notify_one();
    if (windowPresentThread.joinable()) {
      windowPresentThread.join();
    }
    if (closeWindowRequested && g_videoWindow.IsOpen()) {
      g_videoWindow.Close();
    }
  };

  auto maybeLogUiDbg = [&](const std::string& line1, const std::string& line2) {
    if (line1.empty() && line2.empty()) return;
    auto now = std::chrono::steady_clock::now();
    constexpr auto kUiDbgLogInterval = std::chrono::milliseconds(250);
    bool intervalOk =
        (lastUiDbgLog == std::chrono::steady_clock::time_point::min()) ||
        (now - lastUiDbgLog >= kUiDbgLogInterval);
    if (!intervalOk) return;
    bool changed = (line1 != lastUiDbgLine1) || (line2 != lastUiDbgLine2);
    if (!changed) return;
    lastUiDbgLine1 = line1;
    lastUiDbgLine2 = line2;
    lastUiDbgLog = now;
    if (!line1.empty()) {
      perfLogAppendf(&perfLog, "ui_dbg1 %s", line1.c_str());
    }
    if (!line2.empty()) {
      perfLogAppendf(&perfLog, "ui_dbg2 %s", line2.c_str());
    }
  };

  auto renderScreen = [&](bool clearHistory, bool frameChanged) {
    screen.updateSize();
    int width = std::max(20, screen.width());
    int height = std::max(10, screen.height());
    std::string statusLine;
    if (!audioOk && !audioStarting) {
      statusLine = enableAudio ? "Audio unavailable" : "Audio disabled";
    }
    std::string subtitleText;
    std::string debugLine1;
    std::string debugLine2;
#if RADIOIFY_ENABLE_TIMING_LOG
    if (config.debugOverlay) {
      PlayerDebugInfo dbg = player.debugInfo();
      char buf1[256];
      char buf2[256];
      double masterSec =
          static_cast<double>(dbg.masterClockUs) / 1000000.0;
      double diffMs = static_cast<double>(dbg.lastDiffUs) / 1000.0;
      double delayMs = static_cast<double>(dbg.lastDelayUs) / 1000.0;
      std::snprintf(
          buf1, sizeof(buf1),
          "DBG state=%s serial=%d seek=%d qv=%zu master=%s %.3fs diff=%.1fms delay=%.1fms",
          playerStateLabel(dbg.state), dbg.currentSerial,
          dbg.pendingSeekSerial, dbg.videoQueueDepth,
          clockSourceLabel(dbg.masterSource), masterSec, diffMs, delayMs);
      std::snprintf(
          buf2, sizeof(buf2),
          "DBG audio ok=%d ready=%d fresh=%d starved=%d buf=%zuf rate=%u clock=%.3fs",
          dbg.audioOk ? 1 : 0, dbg.audioClockReady ? 1 : 0,
          dbg.audioClockFresh ? 1 : 0, dbg.audioStarved ? 1 : 0,
          dbg.audioBufferedFrames, dbg.audioSampleRate,
          static_cast<double>(dbg.audioClockUs) / 1000000.0);
      debugLine1 = buf1;
      debugLine2 = buf2;
      // maybeLogUiDbg(debugLine1, debugLine2); // REMOVED spammy UI logs
    }
#endif
    int headerLines = 0;
    if (!debugLine1.empty()) {
      headerLines += 1;
    }
    if (!debugLine2.empty()) {
      headerLines += 1;
    }
    if (!statusLine.empty()) {
      headerLines += 1;
    }
    const int footerLines = 0;
    int artTop = headerLines;
    int maxHeight = std::max(1, height - headerLines - footerLines);

    double currentSec = 0.0;
    double totalSec = -1.0;
    int64_t clockUs = player.currentUs();
    if (clockUs > 0) {
      currentSec = static_cast<double>(clockUs) / 1000000.0;
    }
    int64_t durUs = player.durationUs();
    if (durUs > 0) {
      totalSec = static_cast<double>(durUs) / 1000000.0;
    } else if (audioOk) {
      totalSec = audioGetTotalSec();
    }
    if (totalSec > 0.0) {
      currentSec = std::clamp(currentSec, 0.0, totalSec);
    }
    double displaySec = currentSec;
    bool seekingOverlay = player.isSeeking() || localSeekRequested;
    if (seekingOverlay && totalSec > 0.0 && std::isfinite(totalSec)) {
      displaySec = std::clamp(pendingSeekTargetSec, 0.0, totalSec);
    }
    subtitleText = getSubtitleText(clockUs, seekingOverlay);

    bool waitingForAudio =
        audioOk && !audioStreamClockReady() && !audioIsFinished();
    bool audioStarved = audioOk && audioStreamStarved();
    bool waitingForVideo = !player.hasVideoFrame();
    bool isPaused = player.state() == PlayerState::Paused;
    bool allowFrame = haveFrame && !useWindowPresenter;

    auto waitingLabel = [&]() -> std::string {
      if (seekingOverlay) return "Seeking...";
      if (isPaused) return "Paused";
      if (player.state() == PlayerState::Opening) return "Opening...";
      if (player.state() == PlayerState::Prefill) return "Prefilling...";
      if (waitingForAudio) return "Waiting for audio...";
      if (audioStarved || waitingForVideo) return "Buffering video...";
      return "Waiting for video...";
    };

    bool sizeChanged = (width != cachedWidth || maxHeight != cachedMaxHeight ||
                        frame->width != cachedFrameWidth ||
                        frame->height != cachedFrameHeight);

    if (enableAscii && !windowEnabled) {
      if (allowFrame && (frameChanged || clearHistory || sizeChanged)) {
        if (frame->width <= 0 || frame->height <= 0) {
          appendVideoWarning("Skipping video frame with invalid dimensions.");
          haveFrame = false;
          return;
        }
        bool artOk = false;
        try {
          auto [outW, outH] =
              computeAsciiOutputSize(
                  width, maxHeight,
                  ((frame->rotationQuarterTurns & 1) != 0) ? frame->height
                                                            : frame->width,
                  ((frame->rotationQuarterTurns & 1) != 0) ? frame->width
                                                            : frame->height);
          art.width = outW;
          art.height = outH;

          std::string gpuErr;
          bool cacheUpdated = false;
          bool renderRes = false;

          ID3D11Device* device = getSharedGpuDevice();
          if (!device) {
            renderFailed = true;
            renderFailMessage = "GPU device unavailable.";
            renderFailDetail = "Shared GPU device was not initialized.";
            return;
          }
          Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
          device->GetImmediateContext(&context);
          if (!context) {
            renderFailed = true;
            renderFailMessage = "GPU context unavailable.";
            renderFailDetail = "Failed to acquire D3D11 immediate context.";
            return;
          }

          auto t0 = std::chrono::steady_clock::now();
          {
            std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
            if (clearHistory) {
              gpuRenderer.ClearHistory();
            }

            if (frame->format == VideoPixelFormat::HWTexture) {
              if (!frame->hwTexture) {
                renderFailed = true;
                renderFailMessage = "Invalid video frame.";
                renderFailDetail = "Missing hardware texture.";
                return;
              }
              D3D11_TEXTURE2D_DESC desc{};
              frame->hwTexture->GetDesc(&desc);
              bool is10Bit = (desc.Format == DXGI_FORMAT_P010);
              cacheUpdated = g_frameCache.Update(
                  device, context.Get(), frame->hwTexture.Get(),
                  frame->hwTextureArrayIndex, frame->width, frame->height,
                  frame->fullRange, frame->yuvMatrix, frame->yuvTransfer,
                  is10Bit ? 10 : 8, frame->rotationQuarterTurns);
            } else if (frame->format == VideoPixelFormat::NV12 ||
                       frame->format == VideoPixelFormat::P010) {
              if (frame->stride <= 0 || frame->planeHeight <= 0 ||
                  frame->yuv.empty()) {
                renderFailed = true;
                renderFailMessage = "Invalid video frame buffer.";
                renderFailDetail = "Missing YUV plane metadata.";
                return;
              }
              size_t strideBytes = static_cast<size_t>(frame->stride);
              size_t planeHeight = static_cast<size_t>(frame->planeHeight);
              size_t yBytes = strideBytes * planeHeight;
              if (strideBytes == 0 || planeHeight == 0 ||
                  yBytes / strideBytes != planeHeight) {
                renderFailed = true;
                renderFailMessage = "Invalid video frame buffer.";
                renderFailDetail = "Invalid YUV plane sizing.";
                return;
              }
              bool is10Bit = frame->format == VideoPixelFormat::P010;
              cacheUpdated = g_frameCache.UpdateNV12(
                  device, context.Get(), frame->yuv.data(), frame->stride,
                  frame->planeHeight, frame->width, frame->height,
                  frame->fullRange, frame->yuvMatrix, frame->yuvTransfer,
                  is10Bit ? 10 : 8, frame->rotationQuarterTurns);
            } else if (frame->format == VideoPixelFormat::RGB32 ||
                       frame->format == VideoPixelFormat::ARGB32) {
              if (frame->rgba.empty()) {
                renderFailed = true;
                renderFailMessage = "Invalid video frame buffer.";
                renderFailDetail = "Missing RGBA data.";
                return;
              }
              int stride = frame->stride > 0 ? frame->stride : frame->width * 4;
              if (stride <= 0) {
                renderFailed = true;
                renderFailMessage = "Invalid video frame buffer.";
                renderFailDetail = "Invalid RGBA stride.";
                return;
              }
              cacheUpdated = g_frameCache.Update(
                  device, context.Get(), frame->rgba.data(), stride, frame->width,
                  frame->height, frame->rotationQuarterTurns);
            } else {
              renderFailed = true;
              renderFailMessage = "Unsupported video frame format.";
              renderFailDetail = "";
              return;
            }

            if (cacheUpdated) {
              renderRes =
                  gpuRenderer.RenderFromCache(g_frameCache, art, &gpuErr);
            }
          }
          auto t1 = std::chrono::steady_clock::now();
          auto durMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                  .count();
          if (durMs > 50) {
            appendTimingFmt("video_render_slow pts_us=%lld dur_ms=%lld",
                            (long long)(frame->timestamp100ns / 10),
                            (long long)durMs);
          }

          if (renderRes) {
            artOk = true;
            static bool rendererLogged = false;
            if (!rendererLogged) {
              const char* fmt =
                  (frame->format == VideoPixelFormat::HWTexture)
                      ? "hwtexture"
                      : (frame->format == VideoPixelFormat::NV12)
                            ? "nv12"
                            : (frame->format == VideoPixelFormat::P010)
                                  ? "p010"
                                  : "rgba";
              perfLogAppendf(&perfLog,
                             "video_renderer_input format=%s in=%dx%d out=%dx%d",
                             fmt, frame->width, frame->height, outW, outH);
              std::string detail = gpuRenderer.lastNv12TextureDetail();
              if (detail.empty()) {
                detail = std::string("path=") +
                         gpuRenderer.lastNv12TexturePath();
              }
              appendTiming(std::string("video_renderer gpu_active=1 format=") +
                           fmt + " " + detail);
              rendererLogged = true;
            }
          } else if (allowAsciiCpuFallback ||
                     frame->format == VideoPixelFormat::NV12 ||
                     frame->format == VideoPixelFormat::P010 ||
                     frame->format == VideoPixelFormat::RGB32 ||
                     frame->format == VideoPixelFormat::ARGB32) {
            if (!cacheUpdated) {
              appendVideoWarning(
                  "GPU cache update failed; falling back to CPU ASCII path.");
            } else if (!gpuErr.empty()) {
              appendVideoWarning(
                  "GPU ASCII render failed; falling back to CPU path: " +
                  gpuErr);
            } else {
              appendVideoWarning(
                  "GPU ASCII render failed; falling back to CPU path.");
            }
            if (frame->format == VideoPixelFormat::NV12 ||
                frame->format == VideoPixelFormat::P010) {
              bool is10Bit = frame->format == VideoPixelFormat::P010;
              artOk = renderAsciiArtFromYuv(
                  frame->yuv.data(), frame->width, frame->height, frame->stride,
                  frame->planeHeight,
                  is10Bit ? YuvFormat::P010 : YuvFormat::NV12,
                  frame->fullRange, frame->yuvMatrix, frame->yuvTransfer, outW,
                  outH, art);
            } else if (frame->format == VideoPixelFormat::RGB32 ||
                       frame->format == VideoPixelFormat::ARGB32) {
              artOk = renderAsciiArtFromRgba(
                  frame->rgba.data(), frame->width, frame->height, outW, outH,
                  art, frame->format == VideoPixelFormat::ARGB32);
            }
          } else {
            renderFailed = true;
            renderFailMessage = "GPU renderer failed.";
            renderFailDetail =
                gpuErr.empty() ? "GPU cache update or render failed." : gpuErr;
            return;
          }
          if (!artOk) {
            renderFailed = true;
            renderFailMessage = "Failed to render video frame.";
            renderFailDetail = "";
            return;
          }
        } catch (...) {
          renderFailed = true;
          renderFailMessage = "Failed to render video frame.";
          renderFailDetail = "";
          return;
        }
        cachedWidth = width;
        cachedMaxHeight = maxHeight;
        cachedFrameWidth = frame->width;
        cachedFrameHeight = frame->height;
      } else if (!allowFrame) {
        cachedWidth = -1;
        cachedMaxHeight = -1;
        cachedFrameWidth = -1;
        cachedFrameHeight = -1;
      }
    } else {
      if (allowFrame) {
        if (frame->width <= 0 || frame->height <= 0) {
          appendVideoWarning("Skipping video frame with invalid dimensions.");
          haveFrame = false;
          return;
        }
        cachedWidth = width;
        cachedMaxHeight = maxHeight;
        cachedFrameWidth = frame->width;
        cachedFrameHeight = frame->height;
      }
    }

    screen.clear(baseStyle);
    int headerY = 0;
    if (!debugLine1.empty()) {
      screen.writeText(0, headerY++, fitLine(debugLine1, width), dimStyle);
    }
    if (!debugLine2.empty()) {
      screen.writeText(0, headerY++, fitLine(debugLine2, width), dimStyle);
    }
    if (!statusLine.empty()) {
      screen.writeText(0, headerY++, fitLine(statusLine, width), dimStyle);
    }

    if (enableAscii && !windowEnabled) {
      int artWidth = std::min(art.width, width);
      int artHeight = std::min(art.height, maxHeight);
      // Ensure we don't render past screen bounds
      int availableHeight = height - artTop;
      artHeight = std::min(artHeight, availableHeight);
      int artX = std::max(0, (width - artWidth) / 2);

      if (allowFrame && artHeight > 0) {
        for (int y = 0; y < artHeight; ++y) {
          for (int x = 0; x < artWidth; ++x) {
            const auto& cell =
                art.cells[static_cast<size_t>(y * art.width + x)];
            Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
            screen.writeChar(artX + x, artTop + y, cell.ch, cellStyle);
          }
        }
      } else if (!allowFrame) {
        screen.writeText(0, artTop, fitLine(waitingLabel(), width), dimStyle);
      }
    } else {
      std::string label;
      if (windowEnabled) {
        label = "Video window active (W to toggle back)";
        label += windowVsyncEnabled ? "  VSync: On" : "  VSync: Off";
      } else {
        label = allowFrame ? "ASCII rendering disabled" : waitingLabel();
      }
      screen.writeText(0, artTop, fitLine(label, width), dimStyle);
      if (allowFrame && maxHeight > 1) {
        int sizeW = frame->width;
        int sizeH = frame->height;
        if (windowEnabled) {
          int srcW = player.sourceWidth();
          int srcH = player.sourceHeight();
          if (srcW > 0 && srcH > 0) {
            sizeW = srcW;
            sizeH = srcH;
          }
        }
        if (sizeW > 0 && sizeH > 0) {
          std::string sizeLine =
              "Video size: " + std::to_string(sizeW) + "x" +
              std::to_string(sizeH);
          screen.writeText(0, artTop + 1, fitLine(sizeLine, width), dimStyle);
        }
      }
    }

    if (enableAscii && !windowEnabled && !subtitleText.empty()) {
      std::vector<std::string> lines;
      std::string line;
      for (char c : subtitleText) {
        if (c == '\r') continue;
        if (c == '\n') {
          if (!line.empty()) lines.push_back(line);
          line.clear();
        } else {
          line.push_back(c);
        }
      }
      if (!line.empty()) lines.push_back(line);
      if (lines.size() > 2) {
        lines.resize(2);
      }
      if (!lines.empty()) {
        int subtitleBottom = overlayVisible() ? height - 3 : height - 1;
        int maxVisible = subtitleBottom - artTop + 1;
        if (subtitleBottom >= 0 && maxVisible > 0) {
          int visibleLines = std::min<int>(lines.size(), maxVisible);
          int startLine = static_cast<int>(lines.size()) - visibleLines;
          int startY = subtitleBottom - visibleLines + 1;
          for (int i = 0; i < visibleLines; ++i) {
            const std::string& textLine = lines[startLine + i];
            screen.writeText(0, startY + i, fitLine(textLine, width),
                             accentStyle);
          }
        }
      }
    }

    progressBarX = -1;
    progressBarY = -1;
    progressBarWidth = 0;
    if (overlayVisible()) {
      int barLine = height - 1;
      int labelLine = barLine - 1;
      if (labelLine >= artTop && labelLine >= 0) {
        std::string nowLabel = " " + toUtf8String(file.filename());
        screen.writeText(0, labelLine, fitLine(nowLabel, width), accentStyle);
      }

      std::string status;
      bool audioFinished = audioOk && audioIsFinished();
      bool paused = audioOk ? audioIsPaused() : userPaused;
      if (audioFinished) {
        status = "\xE2\x96\xA0";  // ended icon
      } else if (paused) {
        status = "\xE2\x8F\xB8";  // paused icon
      } else {
        status = "\xE2\x96\xB6";  // playing icon
      }
      int volPct = static_cast<int>(std::round(audioGetVolume() * 100.0f));
      float radioGain = audioGetRadioMakeup();
      ProgressTextLayout progressText = buildProgressTextLayout(
          displaySec, totalSec, status, volPct, radioGain, width);
      std::string suffix = progressText.suffix;
      int barWidth = progressText.barWidth;
      double ratio = 0.0;
      if (totalSec > 0.0 && std::isfinite(totalSec)) {
        ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
      }
      progressBarX = 1;
      progressBarY = barLine;
      progressBarWidth = barWidth;
      screen.writeChar(0, barLine, L'|', progressFrameStyle);
      auto barCells = renderProgressBarCells(ratio, barWidth,
                                             progressEmptyStyle, progressStart,
                                             progressEnd);
      for (int i = 0; i < barWidth; ++i) {
        const auto& cell = barCells[static_cast<size_t>(i)];
        screen.writeChar(1 + i, barLine, cell.ch, cell.style);
      }
      screen.writeChar(1 + barWidth, barLine, L'|', progressFrameStyle);
      if (!suffix.empty()) {
        screen.writeText(2 + barWidth, barLine, " " + suffix, baseStyle);
      }
    }

    screen.draw();
  };

  useWindowPresenter = windowThreadEnabled.load(std::memory_order_relaxed);
  renderScreen(true, true);
  if (renderFailed) {
    windowThreadRunning.store(false, std::memory_order_relaxed);
    windowThreadEnabled.store(false, std::memory_order_relaxed);
    windowPresentCv.notify_one();
    player.close();
    stopWindowThread();
    if (audioOk || audioStarting) audioStop();
    g_frameCache.Reset();
    bool ok = reportVideoError(renderFailMessage, renderFailDetail);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  while (running) {
    finalizeAudioStart();

    if (g_videoWindow.IsOpen()) {
      g_videoWindow.PollEvents();
      if (windowEnabled && !g_videoWindow.IsVisible()) {
        windowEnabled = false;
        windowThreadEnabled.store(false, std::memory_order_relaxed);
        windowPresentCv.notify_one();
        forceRefreshArt = true;
        redraw = true;
      }
    }
    
    if (!running) break;

    // UI HEARTBEAT
    static auto lastUiHeartbeat = std::chrono::steady_clock::now();
    auto nowUi = std::chrono::steady_clock::now();
    if (nowUi - lastUiHeartbeat >= std::chrono::seconds(1)) {
        appendTimingFmt("video_heartbeat_ui redraw=%d seeker=%d paused=%d", 
                        redraw ? 1 : 0, localSeekRequested ? 1 : 0, userPaused ? 1 : 0);
        lastUiHeartbeat = nowUi;
    }

    InputEvent ev{};
    auto getNextEvent = [&]() {
        if (input.poll(ev)) return true;
        if (g_videoWindow.IsOpen() && g_videoWindow.PollInput(ev)) {
            // If it's a window mouse event in the bottom area, translate it to a seek
            if (ev.type == InputEvent::Type::Mouse && ev.mouse.buttonState == FROM_LEFT_1ST_BUTTON_PRESSED) {
                // In VideoWindow::WindowProc we ensure y > height * 0.9
                // We'll treat this as a ratio seek directly if possible, or fake a progress bar event.
                // Since handleInputEvent is complex, let's just handle it here.
                double ratio = (double)ev.mouse.pos.X / 1280.0; // This is a bit hacky as we don't know window width here
                // Wait, if I want it to be robust, I should pass the ratio in the event.
                // But InputEvent doesn't have a ratio field.
            }
            return true;
        }
        return false;
    };

    while (getNextEvent()) {
      if (!running) break;
      
      if (ev.type == InputEvent::Type::Resize) {
        pendingResize = true;
        redraw = true;
        continue;
      }
      if (ev.type == InputEvent::Type::Key) {
        InputCallbacks cb;
        cb.onQuit = [&]() { running = false; };
        cb.onTogglePause = [&]() {
          if (audioOk) {
            audioTogglePause();
            userPaused = audioIsPaused();
          } else {
            userPaused = !userPaused;
            player.setVideoPaused(userPaused);
          }
        };
        cb.onToggleRadio = [&]() { if (audioOk) audioToggleRadio(); };
        cb.onToggleVsync = [&]() {
          windowVsyncEnabled = !windowVsyncEnabled;
          g_videoWindow.SetVsync(windowVsyncEnabled);
        };
        cb.onSeekBy = [&](int dir) {
          double currentSec = player.currentUs() / 1000000.0;
          sendSeekRequest(currentSec + dir * 5.0);
        };
        cb.onAdjustVolume = [&](float delta) { audioAdjustVolume(delta); };
        cb.onAdjustRadioMakeup = [&](float delta) {
          audioAdjustRadioMakeup(delta);
          triggerOverlay();
          redraw = true;
          if (windowEnabled) {
            windowForcePresent.store(true, std::memory_order_relaxed);
            windowPresentCv.notify_one();
          }
        };

        if (ev.key.vk == 'W') {
          windowEnabled = !windowEnabled;
          if (windowEnabled) {
            if (!g_videoWindow.IsOpen()) {
              g_videoWindow.Open(1280, 720, "Radioify Output");
            }
            g_videoWindow.ShowWindow(true);
            windowThreadEnabled.store(true, std::memory_order_relaxed);
            windowForcePresent.store(true, std::memory_order_relaxed);
            windowPresentCv.notify_one();
          } else {
            windowThreadEnabled.store(false, std::memory_order_relaxed);
            windowPresentCv.notify_one();
            if (g_videoWindow.IsOpen()) {
              g_videoWindow.ShowWindow(false);
            }
            forceRefreshArt = true;
          }
          triggerOverlay();
          redraw = true;
          continue;
        }

        if (ev.key.vk == VK_ESCAPE || ev.key.vk == VK_BROWSER_BACK ||
            ev.key.vk == VK_BACK) {
          running = false;
          closeWindowRequested = true;
          windowThreadEnabled.store(false, std::memory_order_relaxed);
          windowPresentCv.notify_one();
          continue;
        }

        if (handlePlaybackInput(ev, running, cb)) {
          triggerOverlay();
          redraw = true;
          if (windowEnabled) {
            windowForcePresent.store(true, std::memory_order_relaxed);
            windowPresentCv.notify_one();
          }
          continue;
        }
      }
      if (ev.type == InputEvent::Type::Mouse) {
        const MouseEvent& mouse = ev.mouse;
        if (isBackMousePressed(mouse)) {
          running = false;
          closeWindowRequested = true;
          windowThreadEnabled.store(false, std::memory_order_relaxed);
          windowPresentCv.notify_one();
          continue;
        }

        bool progressHit = isProgressHit(mouse);
        if (progressHit) {
          triggerOverlay();
          redraw = true;
          if (windowEnabled) {
            windowForcePresent.store(true, std::memory_order_relaxed);
            windowPresentCv.notify_one();
          }
        }

        bool leftPressed =
            (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
        
        if (leftPressed && (mouse.control & 0x80000000)) {
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
          continue;
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
              continue;
            }
          }
        }
      }
    }
    if (!running) break;

    finalizeAudioStart();

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
      if (enableAscii) {
        requestTargetSize(width, height);
      }
      pendingResize = false;
      redraw = true;
    }

    bool presented = false;
    VideoFrame nextFrame;
    useWindowPresenter = windowThreadEnabled.load(std::memory_order_relaxed);
    if (!useWindowPresenter && player.tryGetVideoFrame(&nextFrame)) {
      frameBuffer = std::move(nextFrame);
      haveFrame = true;
      presented = true;
      if (!windowEnabled) {
        redraw = true;
      }
    }
    if (!player.hasVideoFrame()) {
      haveFrame = false;
    } else if (useWindowPresenter) {
      haveFrame = true;
    }

    if (localSeekRequested && player.isSeeking()) {
      localSeekRequested = false;
      windowLocalSeekRequested.store(localSeekRequested,
                                     std::memory_order_relaxed);
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
    }
    if (!player.isSeeking() && !localSeekRequested) {
      pendingSeekTargetSec = -1.0;
      windowPendingSeekTargetSec.store(pendingSeekTargetSec,
                                       std::memory_order_relaxed);
    }

    if (player.isEnded()) {
      // Mark ended but keep the loop running so user can seek back without causing
      // the program to immediately exit. This matches ASCII renderer behavior.
      ended = true;
      userPaused = true;
      triggerOverlay();
      redraw = true;
      // do not set running = false here; let user exit explicitly or seek
    }

#if RADIOIFY_ENABLE_TIMING_LOG
    if (redraw || overlayVisible() || config.debugOverlay) {
#else
    if (redraw || overlayVisible()) {
#endif
      auto t0 = std::chrono::steady_clock::now();
      renderScreen(forceRefreshArt, presented);
      auto t1 = std::chrono::steady_clock::now();
      auto durMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
      if (durMs > 100) {
        appendTimingFmt("video_ui_draw_slow dur_ms=%lld", (long long)durMs);
      }
      
      if (renderFailed) {
        running = false;
        break;
      }
      redraw = false;
      forceRefreshArt = false;
    }

#if RADIOIFY_ENABLE_TIMING_LOG
    if (!redraw && !overlayVisible() && !config.debugOverlay) {
#else
    if (!redraw && !overlayVisible()) {
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  if (renderFailed) {
    windowThreadRunning.store(false, std::memory_order_relaxed);
    windowThreadEnabled.store(false, std::memory_order_relaxed);
    windowPresentCv.notify_one();
    player.close();
    stopWindowThread();
    if (audioOk || audioStarting) audioStop();
    g_frameCache.Reset();
    bool ok = reportVideoError(renderFailMessage, renderFailDetail);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  player.close();
  stopWindowThread();
  if (audioOk || audioStarting) audioStop();
  g_frameCache.Reset();
  finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
  return true;
}
