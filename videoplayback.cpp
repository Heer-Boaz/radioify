#include "videoplayback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <new>
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
#include <objbase.h>
#include <roapi.h>

extern "C" {
#include <libavutil/log.h>
}

#include "asciiart.h"
#include "asciiart_gpu.h"
#include "audioplayback.h"
#include "taskgate.h"
#include "ui_helpers.h"
#include "videodecoder.h"

#ifndef RADIOIFY_ENABLE_TIMING_LOG
#define RADIOIFY_ENABLE_TIMING_LOG 1
#endif
#ifndef RADIOIFY_ENABLE_VIDEO_ERROR_LOG
#define RADIOIFY_ENABLE_VIDEO_ERROR_LOG 1
#endif

namespace {
#if RADIOIFY_ENABLE_VIDEO_ERROR_LOG
std::mutex gFfmpegLogMutex;
std::filesystem::path gFfmpegLogPath;

void ffmpegLogCallback(void* ptr, int level, const char* fmt, va_list vl) {
  if (level > AV_LOG_ERROR) return;
  char line[1024];
  int printPrefix = 1;
  av_log_format_line2(ptr, level, fmt, vl, line, sizeof(line), &printPrefix);
  std::filesystem::path path;
  {
    std::lock_guard<std::mutex> lock(gFfmpegLogMutex);
    path = gFfmpegLogPath;
  }
  if (path.empty()) return;
  std::ofstream f(path, std::ios::app);
  if (!f) return;
  f << "ffmpeg_error " << line;
  size_t len = std::strlen(line);
  if (len == 0 || line[len - 1] != '\n') {
    f << "\n";
  }
}
#endif

struct PerfLog {
  std::ofstream file;
  std::string buffer;
  bool enabled = false;
};

GpuAsciiRenderer& sharedGpuRenderer() {
  static GpuAsciiRenderer renderer;
  return renderer;
}

// Returns the shared D3D11 device for zero-copy video decoding
// This ensures the same device is used by both decoder and renderer
ID3D11Device* getSharedGpuDevice() {
  static bool initialized = false;
  static std::mutex initMutex;
  
  GpuAsciiRenderer& renderer = sharedGpuRenderer();
  
  // Lazy init the renderer to get the device
  if (!initialized) {
    std::lock_guard<std::mutex> lock(initMutex);
    if (!initialized) {
      std::string error;
      // Initialize with reasonable defaults - will be resized on first render
      if (renderer.Initialize(1920, 1080, &error)) {
        initialized = true;
      }
    }
  }
  
  return renderer.device();
}

const GateScope kGateFrameFirst{true, "frame", "first"};
const GateScope kGateAudioStart{true, "audio", "start"};
const GateScope kGatePresentFirst{false, "present", "first"};
const GateScope kGateTimestampFirst{false, "timestamp", "first"};
const GateScope kGateAudioPrimed{true, "audio", "primed"};
const GateScope kGateAudioFinished{false, "audio_finished", "ended"};

bool shouldBlockTimestamp(bool check, double rawSec, double leadSlack,
                          double frameSec) {
  return check && (rawSec + leadSlack < frameSec);
}

bool canPresentFrame(const GateGroup& gate) {
  return gate.ready() && gate.readyFor("timestamp");
}

bool waitingForTimestampLabel(const GateGroup& gate) {
  return !gate.readyFor("timestamp");
}

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
#if RADIOIFY_ENABLE_VIDEO_ERROR_LOG
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

  bool fullRedrawEnabled = enableAscii;
  if (fullRedrawEnabled) {
    screen.setAlwaysFullRedraw(true);
  }

  auto showError = [&](const std::string& message,
                       const std::string& detail) -> bool {
    InputEvent ev{};
    while (true) {
      screen.updateSize();
      int width = std::max(20, screen.width());
      screen.clear(baseStyle);
      std::string title = "Video: " + toUtf8String(file.filename());
      screen.writeText(0, 0, fitLine(title, width), accentStyle);
      screen.writeText(0, 1, fitLine("Press any key to return", width),
                       dimStyle);
      std::string line = message;
      std::string extra = detail;
      if (!extra.empty() && extra == line) {
        extra.clear();
      }
      if (line.empty() && !extra.empty()) {
        line = extra;
        extra.clear();
      }
      if (line.empty()) {
        line = "Failed to open video.";
      }
      screen.writeText(0, 3, fitLine(line, width), dimStyle);
      if (!extra.empty()) {
        screen.writeText(0, 4, fitLine(extra, width), dimStyle);
      }
      screen.draw();
      while (input.poll(ev)) {
        if (ev.type == InputEvent::Type::Key) {
          return true;
        }
        if (ev.type == InputEvent::Type::Resize) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  };

  auto showAudioFallbackPrompt = [&](const std::string& message,
                                     const std::string& detail) -> bool {
    InputEvent ev{};
    while (true) {
      screen.updateSize();
      int width = std::max(20, screen.width());
      screen.clear(baseStyle);
      std::string title = "Video: " + toUtf8String(file.filename());
      screen.writeText(0, 0, fitLine(title, width), accentStyle);
      screen.writeText(0, 2, fitLine(message, width), dimStyle);
      if (!detail.empty()) {
        screen.writeText(0, 3, fitLine(detail, width), dimStyle);
      }
      screen.writeText(0, 5,
                       fitLine("Enter: play audio only  Esc/Q: return", width),
                       dimStyle);
      screen.draw();
      while (input.poll(ev)) {
        if (ev.type == InputEvent::Type::Key) {
          if (ev.key.vk == VK_RETURN) return true;
          if (ev.key.vk == VK_ESCAPE || ev.key.vk == 'Q' || ev.key.ch == 'q' ||
              ev.key.ch == 'Q') {
            return false;
          }
        }
        if (ev.type == InputEvent::Type::Resize) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  };

  PerfLog perfLog;
  std::string logError;
  std::filesystem::path logPath =
      std::filesystem::current_path() / "radioify_timing.log";
  configureFfmpegVideoLog(logPath);
  if (!perfLogOpen(&perfLog, logPath, &logError)) {
    bool ok = showError("Failed to open timing log file.", logError);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }
  perfLogAppendf(&perfLog, "video_start file=%s",
                 toUtf8String(file.filename()).c_str());

  // Helper to append simple timing lines from worker threads where direct
  // access to `perfLog` may not be available due to capture/scope issues.
  auto appendTiming = [&](const std::string& s) {
#if RADIOIFY_ENABLE_TIMING_LOG
    std::ofstream f(logPath, std::ios::app);
    if (f) f << s << "\n";
#else
    (void)s;
#endif
  };

  auto appendVideoError = [&](const std::string& message,
                              const std::string& detail) {
#if RADIOIFY_ENABLE_VIDEO_ERROR_LOG
    std::string line = message;
    std::string extra = detail;
    if (!extra.empty() && extra == line) {
      extra.clear();
    }
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
    if (f) f << payload << "\n";
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
    if (f) f << payload << "\n";
#else
    (void)message;
#endif
  };

  auto reportVideoError = [&](const std::string& message,
                              const std::string& detail) -> bool {
    appendVideoError(message, detail);
    return true;
  };

  std::mutex initMutex;
  std::condition_variable initCv;
  bool initDone = false;
  bool initOk = false;
  std::string initError;
  int sourceWidth = 0;
  int sourceHeight = 0;
  int64_t sourceDuration100ns = 0;

  std::mutex decodeErrorMutex;
  std::string decodeError;
  std::atomic<bool> decodeFailed{false};

  bool audioOk = false;
  bool audioStarting = enableAudio;
  bool audioHoldActive = enableAudio;
  double audioSyncBiasSec = 0.0;
  bool audioSyncBiasValid = false;
  bool running = true;
  bool videoEnded = false;
  bool redraw = true;
  AsciiArt art;
  GpuAsciiRenderer& gpuRenderer = sharedGpuRenderer();
  bool gpuAvailable = true;
  VideoFrame* frame = nullptr;
  size_t currentFrameIndex = 0;
  bool haveFrame = false;
  struct QueuedFrame {
    size_t poolIndex = 0;
    VideoReadInfo info{};
    double decodeMs = 0.0;
    uint64_t epoch = 0;
  };
  int cachedWidth = -1;
  int cachedMaxHeight = -1;
  int cachedFrameWidth = -1;
  int cachedFrameHeight = -1;
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
  bool renderFailed = false;
  std::string renderFailMessage;
  std::string renderFailDetail;
  std::mutex scaleWarningMutex;
  std::string scaleWarning;
  std::atomic<bool> scaleWarningPending{false};
  int decoderTargetW = 0;
  int decoderTargetH = 0;
  int requestedTargetW = 0;
  int requestedTargetH = 0;
  bool pendingResize = false;
  bool forceRefreshArt = false;
  uint64_t pendingSeekEpoch = 0;
  double pendingSeekTargetSec = 0.0;
  bool pendingSeekAdjustWallClock = false;
  double pendingSeekHoldSec = 0.0;
  auto pendingSeekHoldUntil = std::chrono::steady_clock::time_point::min();
  uint64_t pendingResizeEpoch = 0;
  bool pendingResizeRepause = false;
  auto resumeGraceUntil = std::chrono::steady_clock::time_point::min();
  bool lastPaused = false;

  std::mutex queueMutex;
  std::condition_variable queueCv;
  std::deque<QueuedFrame> frameQueue;
  constexpr size_t kMaxQueuedFrames = 16;
  constexpr size_t kInvalidPoolIndex = static_cast<size_t>(-1);
  std::vector<VideoFrame> framePool;
  std::deque<size_t> freeFrames;
  std::atomic<size_t> maxQueue{3};
  constexpr bool kEnableSoftwareFallback = false;
  const bool allowDecoderScale = enableAscii;
  struct DecodeCommand {
    bool resize = false;
    int targetW = 0;
    int targetH = 0;
    bool seek = false;
    int64_t seekTs = 0;
    uint64_t epoch = 0;
  };
  std::mutex commandMutex;
  DecodeCommand pendingCommand;
  bool commandPending = false;
  std::atomic<bool> commandPendingAtomic{false};
  std::atomic<uint64_t> commandEpoch{0};
  std::atomic<bool> decodeRunning{false};
  std::atomic<bool> decodeEnded{false};
  std::atomic<bool> decodePaused{false};
  std::atomic<bool> fastForwardPending{false};
  std::atomic<int64_t> fastForwardTargetTs{0};
  std::atomic<uint64_t> readCalls{0};
  std::atomic<uint64_t> skippedCalls{0};
  std::atomic<uint64_t> queueDrops{0};
  std::thread decodeThread;
  uint64_t lastReadCalls = 0;
  uint64_t lastQueueDrops = 0;
  uint64_t lastSkippedCalls = 0;
  double lastSyncDrift = 0.0;
  TaskGate prepGateOwner;
  GateGroup prepGate = prepGateOwner.group("playback");
  GateToken prepFrameToken{};
  GateToken prepAudioToken{};
  GateToken prepPresentToken{};
  GateToken primedToken{};
  GateToken timestampToken{};
  GateToken audioFinishedToken{};
  framePool.resize(kMaxQueuedFrames + 1);
  for (size_t i = 0; i < framePool.size(); ++i) {
    freeFrames.push_back(i);
  }
  prepGate.bump();

  auto clearQueue = [&]() {
    std::lock_guard<std::mutex> lock(queueMutex);
    while (!frameQueue.empty()) {
      freeFrames.push_back(frameQueue.front().poolIndex);
      frameQueue.pop_front();
    }
  };

  auto setDecodeError = [&](const std::string& message) {
    std::lock_guard<std::mutex> lock(decodeErrorMutex);
    decodeError = message;
    decodeFailed.store(true);
  };

  auto setScaleWarning = [&](const std::string& message) {
    {
      std::lock_guard<std::mutex> lock(scaleWarningMutex);
      scaleWarning = message;
    }
    scaleWarningPending.store(true);
  };

  auto getScaleWarning = [&]() {
    std::lock_guard<std::mutex> lock(scaleWarningMutex);
    return scaleWarning;
  };

  auto getDecodeError = [&]() {
    std::lock_guard<std::mutex> lock(decodeErrorMutex);
    return decodeError;
  };

  auto issueCommand = [&](const DecodeCommand& command) {
    {
      std::lock_guard<std::mutex> lock(commandMutex);
      pendingCommand = command;
      commandPending = true;
    }
    commandPendingAtomic.store(true);
    queueCv.notify_all();
  };

  auto tryPopFrame = [&](QueuedFrame& out, uint64_t minEpoch) -> bool {
    std::lock_guard<std::mutex> lock(queueMutex);
    while (!frameQueue.empty()) {
      QueuedFrame candidate = std::move(frameQueue.front());
      frameQueue.pop_front();
      if (candidate.epoch >= minEpoch) {
        out = std::move(candidate);
        return true;
      }
      freeFrames.push_back(candidate.poolIndex);
    }
    return false;
  };

  auto computeTargetSizeForSource = [&](int width, int height, int srcW,
                                        int srcH, bool showSubtitle) {
    int headerLines = showSubtitle ? 1 : 0;
    const int footerLines = 0;
    int maxHeight = std::max(1, height - headerLines - footerLines);
    int maxOutW = std::max(1, width - 8);
    int safeSrcW = std::max(1, srcW);
    int safeSrcH = std::max(1, srcH);
    int outW = std::max(1, std::min(maxOutW, safeSrcW / 2));
    int outH = static_cast<int>(
        std::lround(outW * (static_cast<float>(safeSrcH) / safeSrcW) / 2.0f));
    outH = std::max(1, std::min(outH, safeSrcH / 4));
    if (maxHeight > 0) outH = std::min(outH, maxHeight);
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
    bool showSubtitle = !audioOk;
    return computeTargetSizeForSource(width, height, sourceWidth, sourceHeight,
                                      showSubtitle);
  };

  auto computeAsciiOutputSize = [&](int maxWidth, int maxHeight, int srcW,
                                    int srcH) {
    int safeSrcW = std::max(1, srcW);
    int safeSrcH = std::max(1, srcH);
    int maxOutW = std::max(1, maxWidth - 8);
    int outW = std::max(1, std::min(maxOutW, safeSrcW / 2));
    int outH = static_cast<int>(
        std::lround(outW * (static_cast<float>(safeSrcH) / safeSrcW) / 2.0f));
    outH = std::max(1, std::min(outH, safeSrcH / 4));
    if (maxHeight > 0) outH = std::min(outH, maxHeight);
    return std::pair<int, int>(outW, outH);
  };

  auto requestTargetSize = [&](int width, int height) -> uint64_t {
    auto [targetW, targetH] = computeTargetSize(width, height);
    if (targetW == requestedTargetW && targetH == requestedTargetH) {
      return 0;
    }
    requestedTargetW = targetW;
    requestedTargetH = targetH;
    uint64_t epoch = commandEpoch.fetch_add(1) + 1;
    DecodeCommand cmd;
    cmd.resize = true;
    cmd.targetW = targetW;
    cmd.targetH = targetH;
    cmd.epoch = epoch;
    issueCommand(cmd);
    return epoch;
  };

  auto requestFastForward = [&](int64_t targetTs) {
    fastForwardTargetTs.store(targetTs, std::memory_order_relaxed);
    fastForwardPending.store(true, std::memory_order_relaxed);
    queueCv.notify_all();
  };
  auto setCurrentFrame = [&](size_t poolIndex) {
    size_t releaseIndex = kInvalidPoolIndex;
    if (haveFrame) {
      releaseIndex = currentFrameIndex;
    }
    currentFrameIndex = poolIndex;
    frame = &framePool[currentFrameIndex];
    haveFrame = true;
    if (releaseIndex != kInvalidPoolIndex) {
      std::lock_guard<std::mutex> lock(queueMutex);
      freeFrames.push_back(releaseIndex);
      queueCv.notify_all();
    }
  };

  screen.updateSize();
  const int initScreenWidth = std::max(20, screen.width());
  const int initScreenHeight = std::max(10, screen.height());

  auto stopDecodeThread = [&]() {
    decodeRunning.store(false);
    queueCv.notify_all();
    if (decodeThread.joinable()) {
      decodeThread.join();
    }
  };

  auto startDecodeThread = [&]() {
    if (decodeRunning.load()) return;
    decodeRunning.store(true);
    decodeEnded.store(false);
    decodeFailed.store(false);
    {
      std::lock_guard<std::mutex> lock(decodeErrorMutex);
      decodeError.clear();
    }
    decodeThread = std::thread([&]() {
      HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        {
          std::lock_guard<std::mutex> lock(initMutex);
          initDone = true;
          initOk = false;
          initError = "Failed to initialize COM for video decoding.";
        }
        initCv.notify_all();
        decodeEnded.store(true);
        decodeRunning.store(false);
        queueCv.notify_all();
        return;
      }
      bool comInit = SUCCEEDED(hr);
      HRESULT roHr = RoInitialize(RO_INIT_MULTITHREADED);
      bool roInit = SUCCEEDED(roHr);
      VideoDecoder decoder;
      VideoStreamSelection streamSelection;
      const bool allowRgbOutput = false;
      bool preferHardware = true;
      bool usingSharedDevice = false;
        bool softwareFallbackAttempted = false;
        int64_t lastVideoTs = 0;
        bool haveDecodedFrame = false;
        int currentTargetW = 0;
        int currentTargetH = 0;
        std::string error;

        auto resetStreamSelection = [&]() {
          streamSelection.streams.clear();
          streamSelection.selectedIndex = -1;
        };
        auto logStreamSelection = [&](const VideoStreamSelection& selection) {
          if (selection.streams.empty()) return;
          const VideoStreamInfo* selected = nullptr;
          for (const auto& stream : selection.streams) {
            perfLogAppendf(
                &perfLog,
                "video_stream index=%d size=%dx%d codec=%s bitrate=%lld default=%d attached=%d decoder=%d",
                stream.index, stream.width, stream.height,
                stream.codecName.empty() ? "unknown" : stream.codecName.c_str(),
                static_cast<long long>(stream.bitRate),
                stream.isDefault ? 1 : 0, stream.isAttachedPic ? 1 : 0,
                stream.hasDecoder ? 1 : 0);
            if (stream.index == selection.selectedIndex) {
              selected = &stream;
            }
          }
          if (selection.selectedIndex >= 0 && selected) {
            perfLogAppendf(
                &perfLog,
                "video_stream_selected index=%d size=%dx%d codec=%s bitrate=%lld default=%d",
                selected->index, selected->width, selected->height,
                selected->codecName.empty() ? "unknown"
                                            : selected->codecName.c_str(),
                static_cast<long long>(selected->bitRate),
                selected->isDefault ? 1 : 0);
          }
        };
        
        // Try zero-copy GPU path first (shared device)
        ID3D11Device* sharedDevice = getSharedGpuDevice();
        if (sharedDevice && preferHardware) {
          resetStreamSelection();
          if (decoder.initWithDevice(file, sharedDevice, &error,
                                     &streamSelection)) {
            usingSharedDevice = true;
          } else {
            // Fall back to regular hardware decode
            error.clear();
            resetStreamSelection();
          }
        }
        
        // Regular init if shared device didn't work
        if (!usingSharedDevice &&
            !decoder.init(file, &error, preferHardware, allowRgbOutput,
                          &streamSelection)) {
          {
            std::lock_guard<std::mutex> lock(initMutex);
            initDone = true;
            initOk = false;
            initError = error;
        }
        initCv.notify_all();
        decodeEnded.store(true);
        decodeRunning.store(false);
        queueCv.notify_all();
        if (roInit) {
          RoUninitialize();
        }
        if (comInit) {
          CoUninitialize();
          }
          return;
        }
        if (allowDecoderScale) {
          auto [targetW, targetH] = computeTargetSizeForSource(
              initScreenWidth, initScreenHeight, decoder.width(),
              decoder.height(), false);
          std::string sizeError;
          if (!decoder.setTargetSize(targetW, targetH, &sizeError)) {
            if (sizeError.empty()) {
              sizeError = "Failed to apply target video size.";
            }
            setScaleWarning(sizeError);
          } else {
            setScaleWarning("");
            currentTargetW = targetW;
            currentTargetH = targetH;
          }
        }
        {
          std::lock_guard<std::mutex> lock(initMutex);
          initDone = true;
          initOk = true;
        initError.clear();
        sourceWidth = decoder.width();
        sourceHeight = decoder.height();
        sourceDuration100ns = decoder.duration100ns();
      }
      logStreamSelection(streamSelection);
      initCv.notify_all();

      enum class FallbackResult { NotAttempted, Applied, Failed };
      auto trySoftwareFallback =
          [&](const VideoReadInfo& info) -> FallbackResult {
        if (!kEnableSoftwareFallback) {
          return FallbackResult::NotAttempted;
        }
        if (softwareFallbackAttempted || !preferHardware) {
          return FallbackResult::NotAttempted;
        }
        if (info.noFrameTimeoutMs == 0 && haveDecodedFrame) {
          return FallbackResult::NotAttempted;
        }
        softwareFallbackAttempted = true;
        preferHardware = false;
        std::string fallbackError;
        decoder.uninit();
        if (!decoder.init(file, &fallbackError, preferHardware,
                          allowRgbOutput)) {
          std::string detail = "No video frames after " +
                               std::to_string(info.noFrameTimeoutMs) +
                               "ms; software fallback failed";
          if (!fallbackError.empty()) {
            detail += ": " + fallbackError;
          }
          setDecodeError(detail);
          return FallbackResult::Failed;
        }
        if (currentTargetW > 0 && currentTargetH > 0) {
          std::string sizeError;
          if (!decoder.setTargetSize(currentTargetW, currentTargetH,
                                     &sizeError)) {
            if (sizeError.empty()) {
              sizeError = "Failed to apply target video size.";
            }
            setScaleWarning(sizeError);
          } else {
            setScaleWarning("");
          }
        }
        if (lastVideoTs > 0) {
          decoder.seekToTimestamp100ns(lastVideoTs);
        }
        clearQueue();
        perfLogAppendf(&perfLog, "video_fallback software timeout_ms=%u",
                       info.noFrameTimeoutMs);
        return FallbackResult::Applied;
      };
      auto setNoFrameError = [&](const VideoReadInfo& info) {
        std::string detail = "No video frames after " +
                             std::to_string(info.noFrameTimeoutMs) +
                             "ms; codec may be unsupported.";
        setDecodeError(detail);
      };

      uint64_t currentEpoch = 0;
      int64_t seekingTargetTs = -1;
      while (decodeRunning.load()) {
        DecodeCommand command;
        bool hasCommand = false;
        {
          std::lock_guard<std::mutex> lock(commandMutex);
          if (commandPending) {
            command = pendingCommand;
            commandPending = false;
            hasCommand = true;
            commandPendingAtomic.store(false);
          }
        }
        if (hasCommand) {
          if (command.resize) {
            std::string sizeError;
            if (!decoder.setTargetSize(command.targetW, command.targetH,
                                       &sizeError)) {
              if (sizeError.empty()) {
                sizeError = "Failed to apply target video size.";
              }
              setScaleWarning(sizeError);
            } else {
              setScaleWarning("");
              currentTargetW = command.targetW;
              currentTargetH = command.targetH;
            }
          }
          if (command.seek) {
            if (!decoder.seekToTimestamp100ns(command.seekTs)) {
              setDecodeError("Failed to seek video.");
              decodeEnded.store(true);
              break;
            }
            seekingTargetTs = command.seekTs;
            decodeEnded.store(false);
          }
          currentEpoch = command.epoch;
          clearQueue();
        }
        if (!hasCommand && seekingTargetTs < 0 &&
            (decodePaused.load() || decodeEnded.load())) {
          if (decodeEnded.load()) {
            fastForwardPending.store(false);
          }
          std::unique_lock<std::mutex> lock(queueMutex);
          queueCv.wait_for(lock, std::chrono::milliseconds(30), [&]() {
            return !decodeRunning.load() || commandPendingAtomic.load() ||
                   (!decodePaused.load() && !decodeEnded.load());
          });
          continue;
        }

        if (fastForwardPending.exchange(false) &&
            !commandPendingAtomic.load()) {
          int64_t targetTs =
              fastForwardTargetTs.load(std::memory_order_relaxed);
          size_t droppedFrames = 0;
          {
            std::lock_guard<std::mutex> lock(queueMutex);
            droppedFrames = frameQueue.size();
            while (!frameQueue.empty()) {
              freeFrames.push_back(frameQueue.front().poolIndex);
              frameQueue.pop_front();
            }
          }
          queueCv.notify_all();
          if (droppedFrames > 0) {
            skippedCalls.fetch_add(static_cast<uint64_t>(droppedFrames),
                                   std::memory_order_relaxed);
          }

          size_t skippedLocal = 0;
          VideoFrame skipFrame;
          VideoReadInfo skipInfo{};
          while (decodeRunning.load()) {
            bool ok = decoder.readFrame(skipFrame, &skipInfo, false);
            if (!ok) {
              auto fallback = trySoftwareFallback(skipInfo);
              if (fallback == FallbackResult::Applied) {
                continue;
              }
              if (fallback == FallbackResult::Failed) {
                decodeEnded.store(true);
                break;
              }
              if (!decoder.atEnd() || !haveDecodedFrame) {
                if (skipInfo.noFrameTimeoutMs > 0) {
                  setNoFrameError(skipInfo);
                } else {
                  setDecodeError("Failed to decode video frame.");
                }
              }
              decodeEnded.store(true);
              queueCv.notify_all();
              break;
            }
            readCalls.fetch_add(1, std::memory_order_relaxed);
            ++skippedLocal;
            haveDecodedFrame = true;
            lastVideoTs = skipFrame.timestamp100ns;
            if (skipFrame.timestamp100ns >= targetTs) {
              break;
            }
          }
          if (skippedLocal > 0) {
            skippedCalls.fetch_add(static_cast<uint64_t>(skippedLocal),
                                   std::memory_order_relaxed);
          }
          if (!decodeRunning.load()) {
            break;
          }
          if (decodeEnded.load() || decoder.atEnd()) {
            if (!decoder.atEnd()) {
              break;
            }
            decodeEnded.store(true);
            queueCv.notify_all();
            continue;
          }

          size_t poolIndex = kInvalidPoolIndex;
          {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [&]() {
              return !decodeRunning.load() || commandPendingAtomic.load() ||
                     !freeFrames.empty();
            });
            if (!decodeRunning.load() || commandPendingAtomic.load()) {
              fastForwardPending.store(true);
              continue;
            }
            if (freeFrames.empty()) {
              fastForwardPending.store(true);
              continue;
            }
            poolIndex = freeFrames.front();
            freeFrames.pop_front();
          }

          VideoFrame& decodedFrame = framePool[poolIndex];
          VideoReadInfo info{};
          auto decodeStart = std::chrono::steady_clock::now();
          bool ok = decoder.readFrame(decodedFrame, &info, enableAscii);
          auto decodeEnd = std::chrono::steady_clock::now();
          if (!ok) {
            auto fallback = trySoftwareFallback(info);
            if (fallback == FallbackResult::Applied) {
              {
                std::lock_guard<std::mutex> lock(queueMutex);
                freeFrames.push_back(poolIndex);
              }
              queueCv.notify_all();
              continue;
            }
            if (fallback == FallbackResult::Failed) {
              {
                std::lock_guard<std::mutex> lock(queueMutex);
                freeFrames.push_back(poolIndex);
              }
              queueCv.notify_all();
              decodeEnded.store(true);
              break;
            }
            bool atEnd = decoder.atEnd() && haveDecodedFrame;
            if (!atEnd) {
              if (info.noFrameTimeoutMs > 0) {
                setNoFrameError(info);
              } else {
                setDecodeError("Failed to decode video frame.");
              }
            }
            {
              std::lock_guard<std::mutex> lock(queueMutex);
              freeFrames.push_back(poolIndex);
            }
            queueCv.notify_all();
            decodeEnded.store(true);
            if (atEnd) {
              continue;
            }
            break;
          }
          readCalls.fetch_add(1, std::memory_order_relaxed);
          haveDecodedFrame = true;
          lastVideoTs = info.timestamp100ns;

          double decodeMs =
              std::chrono::duration<double, std::milli>(decodeEnd - decodeStart)
                  .count();
          {
            std::lock_guard<std::mutex> lock(queueMutex);
            size_t queueLimit = maxQueue.load(std::memory_order_relaxed);
            if (frameQueue.size() >= queueLimit) {
              freeFrames.push_back(frameQueue.front().poolIndex);
              frameQueue.pop_front();
              queueDrops.fetch_add(1, std::memory_order_relaxed);
            }
            frameQueue.push_back(
                QueuedFrame{poolIndex, info, decodeMs, currentEpoch});
          }
          queueCv.notify_one();
          continue;
        }

        size_t poolIndex = kInvalidPoolIndex;
        {
          std::unique_lock<std::mutex> lock(queueMutex);
          size_t queueLimit = maxQueue.load(std::memory_order_relaxed);
          if (frameQueue.size() >= queueLimit || freeFrames.empty()) {
            queueCv.wait_for(lock, std::chrono::milliseconds(30), [&]() {
              return !decodeRunning.load() ||
                     (decodePaused.load() && seekingTargetTs < 0) ||
                     commandPendingAtomic.load() ||
                     (frameQueue.size() <
                          maxQueue.load(std::memory_order_relaxed) &&
                      !freeFrames.empty());
            });
            continue;
          }
          poolIndex = freeFrames.front();
          freeFrames.pop_front();
        }

        VideoFrame& decodedFrame = framePool[poolIndex];
        VideoReadInfo info{};
        auto decodeStart = std::chrono::steady_clock::now();

        bool skipping = (seekingTargetTs >= 0);
        bool decodePixels = enableAscii && !skipping;

        bool ok = decoder.readFrame(decodedFrame, &info, decodePixels);

        if (ok && skipping) {
          if (info.timestamp100ns < seekingTargetTs) {
            lastVideoTs = info.timestamp100ns;
            {
              std::lock_guard<std::mutex> lock(queueMutex);
              freeFrames.push_back(poolIndex);
            }
            continue;
          }
          seekingTargetTs = -1;
          if (enableAscii) {
            if (!decoder.redecodeLastFrame(decodedFrame)) {
              ok = false;
            }
          }
        }

        auto decodeEnd = std::chrono::steady_clock::now();
        if (!ok) {
          auto fallback = trySoftwareFallback(info);
          if (fallback == FallbackResult::Applied) {
            {
              std::lock_guard<std::mutex> lock(queueMutex);
              freeFrames.push_back(poolIndex);
            }
            queueCv.notify_all();
            continue;
          }
          if (fallback == FallbackResult::Failed) {
            {
              std::lock_guard<std::mutex> lock(queueMutex);
              freeFrames.push_back(poolIndex);
            }
            queueCv.notify_all();
            decodeEnded.store(true);
            break;
          }
          bool atEnd = decoder.atEnd() && haveDecodedFrame;
          if (!atEnd) {
            if (info.noFrameTimeoutMs > 0) {
              setNoFrameError(info);
            } else {
              setDecodeError("Failed to decode video frame.");
            }
          }
          {
            std::lock_guard<std::mutex> lock(queueMutex);
            freeFrames.push_back(poolIndex);
          }
          queueCv.notify_all();
          decodeEnded.store(true);
          if (atEnd) {
            continue;
          }
          break;
        }
        readCalls.fetch_add(1, std::memory_order_relaxed);
        haveDecodedFrame = true;
        lastVideoTs = info.timestamp100ns;

        double decodeMs =
            std::chrono::duration<double, std::milli>(decodeEnd - decodeStart)
                .count();
        {
          std::lock_guard<std::mutex> lock(queueMutex);
          size_t queueLimit = maxQueue.load(std::memory_order_relaxed);
          if (frameQueue.size() >= queueLimit) {
            freeFrames.push_back(frameQueue.front().poolIndex);
            frameQueue.pop_front();
            queueDrops.fetch_add(1, std::memory_order_relaxed);
          }
          frameQueue.push_back(QueuedFrame{poolIndex, info, decodeMs,
                                           currentEpoch});
        }
        queueCv.notify_one();
      }
      decodeRunning.store(false);
      decodeEnded.store(true);
      queueCv.notify_all();
      if (roInit) {
        RoUninitialize();
      }
      if (comInit) {
        CoUninitialize();
      }
    });
  };

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

  startDecodeThread();
  auto initStart = std::chrono::steady_clock::now();
  auto lastInitDraw = std::chrono::steady_clock::time_point::min();
  while (running) {
    {
      std::unique_lock<std::mutex> lock(initMutex);
      if (initDone) {
        break;
      }
      initCv.wait_for(lock, std::chrono::milliseconds(30),
                      [&]() { return initDone; });
      if (initDone) {
        break;
      }
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
        if (key.vk == VK_ESCAPE || key.vk == 'Q' || key.ch == 'q' ||
            key.ch == 'Q') {
          running = false;
          break;
        }
      } else if (ev.type == InputEvent::Type::Resize) {
        lastInitDraw = std::chrono::steady_clock::time_point::min();
      }
    }
  }
  if (!running) {
    stopDecodeThread();
    if (audioOk) audioStop();
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return true;
  }
  if (!initOk) {
    stopDecodeThread();
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

  VideoReadInfo firstInfo{};
  uint64_t pendingScaleEpoch = 0;

  QueuedFrame firstQueued{};
  bool haveFirstQueued = false;
  constexpr size_t kPrepMinBufferedFrames = 2;
  constexpr auto kPrepMaxWait = std::chrono::milliseconds(600);
  auto prepStart = std::chrono::steady_clock::now();
  auto lastPrepDraw = std::chrono::steady_clock::time_point::min();
  while (running) {
    if (!haveFirstQueued && tryPopFrame(firstQueued, pendingScaleEpoch)) {
      haveFirstQueued = true;
    }
    prepGate.ensure(prepFrameToken, !haveFirstQueued, kGateFrameFirst);
    if (haveFirstQueued) {
      size_t bufferedFrames = 0;
      {
        std::lock_guard<std::mutex> lock(queueMutex);
        bufferedFrames = frameQueue.size();
      }
      bufferedFrames += 1;
      if (bufferedFrames >= kPrepMinBufferedFrames ||
          std::chrono::steady_clock::now() - prepStart >= kPrepMaxWait) {
        break;
      }
    }
    if (decodeEnded.load()) {
      break;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - lastPrepDraw >= kPrepRedrawInterval) {
      double elapsed = std::chrono::duration<double>(now - prepStart).count();
      double phase = std::fmod(elapsed, kPrepPulseSeconds);
      double halfPulse = kPrepPulseSeconds * 0.5;
      double ratio =
          (phase <= halfPulse) ? (phase / halfPulse)
                               : ((kPrepPulseSeconds - phase) / halfPulse);
      renderPreparingScreen(ratio);
      lastPrepDraw = now;
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
        if (key.vk == VK_ESCAPE || key.vk == 'Q' || key.ch == 'q' ||
            key.ch == 'Q') {
          running = false;
          break;
        }
      } else if (ev.type == InputEvent::Type::Resize) {
        lastPrepDraw = std::chrono::steady_clock::time_point::min();
      }
    }
    if (!running) {
      break;
    }
    std::unique_lock<std::mutex> lock(queueMutex);
    queueCv.wait_for(lock, std::chrono::milliseconds(30), [&]() {
      return !frameQueue.empty() || decodeEnded.load();
    });
  }
  if (!running) {
    stopDecodeThread();
    if (audioOk) audioStop();
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return true;
  }
  if (!haveFirstQueued) {
    stopDecodeThread();
    if (audioOk) audioStop();
    std::string fail = getDecodeError();
    if (fail.empty()) {
      fail = "No video frames found.";
    }
    bool ok = showError(fail, "");
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }
  setCurrentFrame(firstQueued.poolIndex);
  prepPresentToken = prepGate.begin(kGatePresentFirst);
  firstInfo = firstQueued.info;
  decoderTargetW = frame->width;
  decoderTargetH = frame->height;
  if (pendingScaleEpoch != 0) {
    perfLogAppendf(&perfLog, "video_scale target=%dx%d actual=%dx%d",
                   requestedTargetW, requestedTargetH, decoderTargetW,
                   decoderTargetH);
  } else {
    requestedTargetW = decoderTargetW;
    requestedTargetH = decoderTargetH;
  }
  perfLogAppendf(
      &perfLog,
      "first_frame ts100ns=%lld dur100ns=%lld flags=0x%X rec=%u ehr=0x%08X "
      "size=%dx%d",
      static_cast<long long>(frame->timestamp100ns),
      static_cast<long long>(firstInfo.duration100ns), firstInfo.flags,
      firstInfo.recoveries, firstInfo.errorHr, frame->width, frame->height);

  const double ticksToSeconds = 1.0 / 10000000.0;
  const double secondsToTicks = 10000000.0;
  const double kLeadSlackSec = 0.005;
  const double kPresentFps = 30.0;
  const double kPresentIntervalSec = 1.0 / kPresentFps;
  const double syncTolerance = std::max(0.033, 0.75 * kPresentIntervalSec);
  const double hardResyncThreshold =
      std::max(0.5, 10.0 * kPresentIntervalSec);
  double nextPresentSec = 0.0;
  bool presentInit = false;
  bool presentUseWall = false;
  bool resetPresentClock = false;
  int64_t firstTs = frame->timestamp100ns;
  double audioStartOffsetSec =
      std::max(0.0, static_cast<double>(firstTs) * ticksToSeconds);
  double frameSec =
      static_cast<double>(frame->timestamp100ns - firstTs) * ticksToSeconds;
  double lastFrameSec = frameSec;
  double lastFrameDurationSec =
      firstInfo.duration100ns > 0
          ? static_cast<double>(firstInfo.duration100ns) * ticksToSeconds
          : 0.0;
  auto updateMaxQueue = [&](double frameDurSec) {
    if (frameDurSec <= 0.0) return;
    size_t needed =
        static_cast<size_t>(std::ceil(kPresentIntervalSec / frameDurSec)) + 2;
    if (needed < 3) needed = 3;
    if (needed > kMaxQueuedFrames) needed = kMaxQueuedFrames;
    maxQueue.store(needed, std::memory_order_relaxed);
  };
  updateMaxQueue(lastFrameDurationSec);
  // Don't initialize sync offset yet - wait for first real frame display
  // This ensures audio has had time to start properly
  auto startTime = std::chrono::steady_clock::now();
  auto lastUiUpdate = startTime;
  auto lastLogTime = startTime;
  double lastSleepMs = 0.0;

  auto totalDurationSec = [&]() -> double {
    double total =
        sourceDuration100ns > 0 ? sourceDuration100ns * ticksToSeconds : -1.0;
    if (total <= 0.0 && audioOk) {
      total = audioGetTotalSec();
    }
    if (total > 0.0 && audioStartOffsetSec > 0.0) {
      total = std::max(0.0, total - audioStartOffsetSec);
    }
    return total;
  };

  auto currentTimeSec = [&]() -> double {
    if (audioOk) {
      double raw = audioGetTimeSec();
      double bias =
          audioSyncBiasValid ? audioSyncBiasSec : audioStartOffsetSec;
      double adjusted = raw - bias;
      return std::max(0.0, adjusted);
    }
    return lastFrameSec;
  };

  constexpr auto kProgressOverlayTimeout = std::chrono::milliseconds(2000);
  auto overlayUntil = std::chrono::steady_clock::time_point::min();
  auto triggerOverlay = [&]() {
    overlayUntil = std::chrono::steady_clock::now() + kProgressOverlayTimeout;
  };
  auto overlayVisible = [&]() {
    if (overlayUntil == std::chrono::steady_clock::time_point::min()) {
      return false;
    }
    return std::chrono::steady_clock::now() <= overlayUntil;
  };

  auto renderScreen = [&](bool refreshArt) {
    screen.updateSize();
    int width = std::max(20, screen.width());
    int height = std::max(10, screen.height());
    std::string subtitle;
    if (!audioOk && !audioStarting) {
      subtitle = enableAudio ? "Audio unavailable" : "Audio disabled";
    }
    int headerLines = 0;
    if (!subtitle.empty()) {
      headerLines += 1;
    }
    const int footerLines = 0;
    int artTop = headerLines;
    int maxHeight = std::max(1, height - headerLines - footerLines);

    double currentSec = currentTimeSec();
    double rawSec = audioOk ? audioGetTimeSec() : currentSec;
    double gateSec = rawSec;
    if (audioOk) {
      double gateBias =
          audioSyncBiasValid ? audioSyncBiasSec : audioStartOffsetSec;
      gateSec = std::max(0.0, rawSec - gateBias);
    }
    double totalSec = totalDurationSec();
    if (totalSec > 0.0) {
      currentSec = std::clamp(currentSec, 0.0, totalSec);
    }
    double displaySec = currentSec;
    auto displayNow = std::chrono::steady_clock::now();
    if (pendingSeekEpoch != 0 && totalSec > 0.0 && std::isfinite(totalSec)) {
      displaySec = std::clamp(pendingSeekTargetSec, 0.0, totalSec);
    } else if (pendingSeekHoldUntil !=
                   std::chrono::steady_clock::time_point::min() &&
               displayNow < pendingSeekHoldUntil && totalSec > 0.0 &&
               std::isfinite(totalSec)) {
      displaySec = std::clamp(pendingSeekHoldSec, 0.0, totalSec);
    }
    bool audioPrimed = audioOk ? audioIsPrimed() : true;
    prepGate.ensure(primedToken, audioOk && !audioPrimed, kGateAudioPrimed);

    bool shouldCheckTimestamp =
        prepPresentToken.active && audioOk && !audioHoldActive;
    bool timestampBlocked =
        shouldBlockTimestamp(shouldCheckTimestamp, gateSec, kLeadSlackSec,
                             frameSec);
    prepGate.ensure(timestampToken, shouldCheckTimestamp && timestampBlocked,
                    kGateTimestampFirst);

    prepGate.ensure(audioFinishedToken, audioOk && audioIsFinished(),
                    kGateAudioFinished);

    bool preparingPlayback = !prepGate.ready();
    bool waitingForTimestamp = waitingForTimestampLabel(prepGate);
    bool allowFrame = canPresentFrame(prepGate);
    // Audio hold release moved to end of function to ensure video is rendered first
    prepGate.ensure(prepPresentToken, !allowFrame, kGatePresentFirst);

    bool sizeChanged =
        (width != cachedWidth || maxHeight != cachedMaxHeight ||
         frame->width != cachedFrameWidth || frame->height != cachedFrameHeight);
    if (enableAscii) {
      if (allowFrame && (refreshArt || sizeChanged)) {
        if (frame->width <= 0 || frame->height <= 0) {
          renderFailed = true;
          renderFailMessage = "Invalid video frame size.";
          renderFailDetail = "";
          return;
        }
        bool artOk = false;
        try {
          // Zero-copy GPU texture path (HWTexture from shared device decoder)
          if (frame->format == VideoPixelFormat::HWTexture && frame->hwTexture) {
            if (gpuAvailable) {
              auto [outW, outH] =
                  computeAsciiOutputSize(width, maxHeight, frame->width,
                                         frame->height);
              art.width = outW;
              art.height = outH;
              std::string gpuErr;
              
              if (gpuRenderer.RenderNV12Texture(frame->hwTexture.Get(), frame->hwTextureArrayIndex,
                                                 frame->width, frame->height,
                                                 frame->fullRange, frame->yuvMatrix,
                                                 frame->yuvTransfer, false, art, &gpuErr)) {
                artOk = true;
                static bool hwTextureLogged = false;
                if (!hwTextureLogged) {
                  perfLogAppendf(&perfLog,
                                 "video_renderer_input format=hwtexture in=%dx%d out=%dx%d",
                                 frame->width, frame->height, outW, outH);
                  appendTiming("video_renderer gpu_active=1 format=hwtexture zero_copy=1");
                  hwTextureLogged = true;
                }
              } else {
                gpuAvailable = false;
                appendVideoWarning("GPU renderer failed (HWTexture), falling back to CPU: " +
                                   gpuErr);
                throw std::runtime_error("ASCII-renderer GPU failure" + gpuErr);
              }
            }
          } else if (frame->format == VideoPixelFormat::NV12 ||
              frame->format == VideoPixelFormat::P010) {
            if (frame->stride <= 0 || frame->planeHeight <= 0) {
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
              renderFailDetail = "Invalid YUV plane layout.";
              return;
            }
            size_t required = yBytes + yBytes / 2;
            if (required < yBytes || frame->yuv.size() < required) {
              renderFailed = true;
              renderFailMessage = "Invalid video frame buffer.";
              renderFailDetail = "Frame pixel data is missing.";
              return;
            }

            bool renderedWithGpu = false;
            if (gpuAvailable &&
                (frame->format == VideoPixelFormat::NV12 ||
                 frame->format == VideoPixelFormat::P010)) {
              auto [outW, outH] =
                  computeAsciiOutputSize(width, maxHeight, frame->width,
                                         frame->height);
              art.width = outW;
              art.height = outH;
              std::string gpuErr;
              
              // Stats calculation moved to GPU (StatsCS)
              if (gpuRenderer.RenderNV12(frame->yuv.data(), frame->width,
                                         frame->height, frame->stride,
                                         frame->planeHeight, frame->fullRange, 
                                         frame->yuvMatrix, frame->yuvTransfer,
                                         frame->format == VideoPixelFormat::P010,
                                         art, &gpuErr)) {
                renderedWithGpu = true;
                artOk = true;
                static bool gpuLogged = false;
                if (!gpuLogged) {
                  const char* inputFormat =
                      (frame->format == VideoPixelFormat::P010) ? "p010" : "nv12";
                  perfLogAppendf(&perfLog,
                                 "video_renderer_input format=%s in=%dx%d out=%dx%d",
                                 inputFormat, frame->width, frame->height, outW, outH);
                  appendTiming("video_renderer gpu_active=1 format=nv12");
                  gpuLogged = true;
                }
              } else {
                gpuAvailable = false;
                appendVideoWarning("GPU renderer failed (NV12), falling back to CPU: " +
                                   gpuErr);
                throw std::runtime_error("ASCII-renderer GPU failure" + gpuErr);
              }
            }

            if (!renderedWithGpu) {
              YuvFormat yuvFormat = (frame->format == VideoPixelFormat::P010)
                                        ? YuvFormat::P010
                                        : YuvFormat::NV12;
              artOk = renderAsciiArtFromYuv(
                  frame->yuv.data(), frame->width, frame->height, frame->stride,
                  frame->planeHeight, yuvFormat, frame->fullRange,
                  frame->yuvMatrix, frame->yuvTransfer, width, maxHeight, art);
            }
          } else {
            size_t expected = static_cast<size_t>(frame->width) *
                              static_cast<size_t>(frame->height) * 4u;
            if (frame->rgba.size() < expected) {
              renderFailed = true;
              renderFailMessage = "Invalid video frame buffer.";
              renderFailDetail = "Frame pixel data is missing.";
              return;
            }

            bool renderedWithGpu = false;
            if (gpuAvailable) {
              auto [outW, outH] =
                  computeAsciiOutputSize(width, maxHeight, frame->width,
                                         frame->height);
              art.width = outW;
              art.height = outH;
              std::string gpuErr;
              if (gpuRenderer.Render(frame->rgba.data(), frame->width,
                                     frame->height, art, &gpuErr)) {
                renderedWithGpu = true;
                artOk = true;
                static bool gpuLogged = false;
                if (!gpuLogged) {
                  appendTiming("video_renderer gpu_active=1");
                  gpuLogged = true;
                }
              } else {
                // If GPU fails, disable it for this session and fall back
                gpuAvailable = false;
                appendVideoWarning("GPU renderer failed, falling back to CPU: " +
                                   gpuErr);
              }
            }

            if (!renderedWithGpu) {
              artOk = renderAsciiArtFromRgba(frame->rgba.data(), frame->width,
                                             frame->height, width, maxHeight,
                                             art, true);
            }
          }
        } catch (const std::bad_alloc&) {
          renderFailed = true;
          renderFailMessage = "Failed to render video frame.";
          renderFailDetail = "Out of memory.";
          return;
        } catch (...) {
          renderFailed = true;
          renderFailMessage = "Failed to render video frame.";
          renderFailDetail = "";
          return;
        }
        if (!artOk) {
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
      if (frame->width <= 0 || frame->height <= 0) {
        renderFailed = true;
        renderFailMessage = "Invalid video frame size.";
        renderFailDetail = "";
        return;
      }
      cachedWidth = width;
      cachedMaxHeight = maxHeight;
      cachedFrameWidth = frame->width;
      cachedFrameHeight = frame->height;
    }

    screen.clear(baseStyle);
    if (!subtitle.empty()) {
      screen.writeText(0, 0, fitLine(subtitle, width), dimStyle);
    }

    if (enableAscii) {
      int artWidth = std::min(art.width, width);
      int artHeight = std::min(art.height, maxHeight);
      int artX = std::max(0, (width - artWidth) / 2);

      if (allowFrame) {
        for (int y = 0; y < artHeight; ++y) {
          for (int x = 0; x < artWidth; ++x) {
            const auto& cell =
                art.cells[static_cast<size_t>(y * art.width + x)];
            Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
            screen.writeChar(artX + x, artTop + y, cell.ch, cellStyle);
          }
        }
      } else if (pendingSeekEpoch != 0 && art.width > 0 && art.height > 0) {
        // Keep showing the last frame while seeking
        for (int y = 0; y < artHeight; ++y) {
          for (int x = 0; x < artWidth; ++x) {
            const auto& cell =
                art.cells[static_cast<size_t>(y * art.width + x)];
            Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
            screen.writeChar(artX + x, artTop + y, cell.ch, cellStyle);
          }
        }
      } else {
        std::string label = preparingPlayback
                                ? "Preparing video playback..."
                                : (waitingForTimestamp
                                       ? "Waiting for video timestamp..."
                                       : "Waiting for video...");
        screen.writeText(0, artTop, fitLine(label, width), dimStyle);
      }
    } else {
      std::string label = allowFrame
                              ? "ASCII rendering disabled"
                              : (preparingPlayback ? "Preparing video playback..."
                                                   : (waitingForTimestamp
                                                          ? "Waiting for video timestamp..."
                                                          : "Waiting for video..."));
      screen.writeText(0, artTop, fitLine(label, width), dimStyle);
      if (maxHeight > 1) {
        std::string sizeLine =
            "Video size: " + std::to_string(frame->width) + "x" +
            std::to_string(frame->height);
        screen.writeText(0, artTop + 1, fitLine(sizeLine, width), dimStyle);
      }
    }

    if (allowFrame && audioHoldActive) {
      audioSetHold(false);
      audioHoldActive = false;
      audioSyncBiasSec = 0.0;
      audioSyncBiasValid = false;
      resetPresentClock = true;
      redraw = true;
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
      bool audioFinished = audioOk && !prepGate.readyFor("audio_finished");
      if (audioFinished) {
        status = "\xE2\x96\xA0";  // ended icon
      } else if (audioOk && audioIsPaused()) {
        status = "\xE2\x8F\xB8";  // paused icon
      } else {
        status = "\xE2\x96\xB6";  // playing icon
      }
      std::string suffix =
          formatTime(displaySec) + " / " + formatTime(totalSec) + " " + status;
      int suffixWidth = utf8CodepointCount(suffix);
      int barWidth = width - suffixWidth - 3;
      if (barWidth < 10) {
        suffix = formatTime(displaySec) + "/" + formatTime(totalSec);
        suffixWidth = utf8CodepointCount(suffix);
        barWidth = width - suffixWidth - 3;
      }
      if (barWidth < 10) {
        suffix = formatTime(displaySec);
        suffixWidth = utf8CodepointCount(suffix);
        barWidth = width - suffixWidth - 3;
      }
      if (barWidth < 5) {
        suffix.clear();
        barWidth = width - 2;
      }
      int maxBar = std::max(5, width - 2);
      barWidth = std::clamp(barWidth, 5, maxBar);
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

  std::atomic<bool> audioStartDone{false};
  std::atomic<bool> audioStartOk{false};
  std::thread audioStartThread;
  if (enableAudio) {
    prepAudioToken = prepGate.begin(kGateAudioStart);
    if (audioHoldActive) {
      audioSetHold(true);
    }
    audioStartThread = std::thread([&]() {
      bool ok = audioStartFileAt(file, audioStartOffsetSec);
      audioStartOk.store(ok);
      audioStartDone.store(true);
    });
  } else {
    audioStartOk.store(false);
    audioStartDone.store(true);
    audioStarting = false;
  }

  auto finalizeAudioStart = [&]() {
    if (!audioStarting || !audioStartDone.load()) return;
    if (audioStartThread.joinable()) {
      audioStartThread.join();
    }
    audioOk = audioStartOk.load();
    audioStarting = false;
    audioSyncBiasSec = 0.0;
    audioSyncBiasValid = false;
    if (prepAudioToken.active) {
      prepGate.end(prepAudioToken);
    }
    if (!audioOk) {
      audioSetHold(false);
      audioHoldActive = false;
    }
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
    resetPresentClock = true;
    redraw = true;
  };

  // finalizeAudioStart(); // Moved to main loop to avoid blocking UI
  renderScreen(true);
  if (renderFailed) {
    if (audioStartThread.joinable()) {
      audioStartThread.join();
    }
    if (audioStarting) {
      audioOk = audioStartOk.load();
      audioStarting = false;
    }
    stopDecodeThread();
    if (audioOk) audioStop();
    bool ok = reportVideoError(renderFailMessage, renderFailDetail);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  while (running) {
    if (audioStarting && audioStartDone.load()) {
      finalizeAudioStart();
    }

    InputEvent ev{};
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Resize) {
        pendingResize = true;
        redraw = true;
        continue;
      }
      if (ev.type == InputEvent::Type::Key) {
        const KeyEvent& key = ev.key;
        const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
        bool ctrl = (key.control & ctrlMask) != 0;
        if ((key.vk == 'C' || key.ch == 'c' || key.ch == 'C') && ctrl) {
          running = false;
          break;
        }
        if (key.vk == VK_ESCAPE || key.vk == 'Q' || key.ch == 'q' ||
            key.ch == 'Q') {
          running = false;
          break;
        }
        if (key.vk == VK_SPACE || key.ch == ' ') {
          if (audioOk) audioTogglePause();
          redraw = true;
        } else if (key.vk == 'R' || key.ch == 'r' || key.ch == 'R') {
          if (audioOk) audioToggleRadio();
          redraw = true;
        } else if (ctrl && (key.vk == VK_LEFT || key.vk == VK_RIGHT)) {
          triggerOverlay();
          int dir = (key.vk == VK_LEFT) ? -1 : 1;
          double currentSec = currentTimeSec();
          double totalSec = totalDurationSec();
          double targetSec = currentSec + dir * 5.0;
          if (totalSec > 0.0) {
            targetSec = std::clamp(targetSec, 0.0, totalSec);
          } else if (targetSec < 0.0) {
            targetSec = 0.0;
          }
          if (audioOk) {
            audioSeekToSec(audioStartOffsetSec + targetSec);
          }
          int64_t targetTs =
              firstTs + static_cast<int64_t>(targetSec * secondsToTicks);
          uint64_t epoch = commandEpoch.fetch_add(1) + 1;
          DecodeCommand cmd;
          cmd.seek = true;
          cmd.seekTs = targetTs;
          cmd.epoch = epoch;
          issueCommand(cmd);
          pendingSeekEpoch = epoch;
          pendingSeekTargetSec = targetSec;
          pendingSeekAdjustWallClock = !audioOk;
          audioSyncBiasValid = false;
          videoEnded = false;
          redraw = true;
          forceRefreshArt = true;
          resetPresentClock = true;
        }
      }
      if (ev.type == InputEvent::Type::Mouse) {
        triggerOverlay();
        redraw = true;
        const MouseEvent& mouse = ev.mouse;
        bool leftPressed =
            (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
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
              double totalSec = totalDurationSec();
              if (totalSec > 0.0 && std::isfinite(totalSec)) {
                double targetSec = ratio * totalSec;
                if (audioOk) {
                  audioSeekToSec(audioStartOffsetSec + targetSec);
                }
                int64_t targetTs =
                    firstTs + static_cast<int64_t>(targetSec * secondsToTicks);
                uint64_t epoch = commandEpoch.fetch_add(1) + 1;
                DecodeCommand cmd;
                cmd.seek = true;
                cmd.seekTs = targetTs;
                cmd.epoch = epoch;
                issueCommand(cmd);
                pendingSeekEpoch = epoch;
                pendingSeekTargetSec = targetSec;
                pendingSeekAdjustWallClock = !audioOk;
                audioSyncBiasValid = false;
                videoEnded = false;
                forceRefreshArt = true;
                resetPresentClock = true;
                redraw = true;
              }
              continue;
            }
          }
        }
      }
    }
    if (!running) break;

    finalizeAudioStart();

    if (scaleWarningPending.exchange(false)) {
      std::string warning = getScaleWarning();
      if (!warning.empty()) {
        appendVideoWarning(warning);
      }
    }

    auto now = std::chrono::steady_clock::now();
    double wallclockElapsed =
        std::chrono::duration<double>(now - startTime).count();
    if (now - lastUiUpdate >= std::chrono::milliseconds(150)) {
      redraw = true;
      lastUiUpdate = now;
    }

    bool paused = audioOk && audioIsPaused();
    if (pendingResizeRepause) {
      decodePaused.store(false);
    } else {
      decodePaused.store(paused);
    }
    queueCv.notify_all();
    if (lastPaused && !paused) {
      resumeGraceUntil = now + std::chrono::milliseconds(500);
      resetPresentClock = true;
    }
    lastPaused = paused;
    double syncSec = 0.0;
    double audioNow = 0.0;
    double audioNowRaw = 0.0;
    bool haveAudioClock = audioOk;
    bool audioPrimed = audioOk ? audioIsPrimed() : true;
    if (haveAudioClock) {
      audioNowRaw = audioGetTimeSec();
      if (audioSyncBiasValid) {
        audioNow = std::max(0.0, audioNowRaw - audioSyncBiasSec);
      } else {
        audioNow = std::max(0.0, audioNowRaw - audioStartOffsetSec);
      }
      syncSec = std::max(0.0, audioNow);
    } else {
      syncSec = std::chrono::duration<double>(now - startTime).count();
    }
    const double leadSlack = kLeadSlackSec;
    bool waitingForAudioStart =
        haveAudioClock && !audioPrimed && audioNow <= 0.0;
    bool useWallClock = paused || waitingForAudioStart;
    double presentClockSec = useWallClock ? wallclockElapsed : syncSec;
    if (!presentInit || useWallClock != presentUseWall || resetPresentClock) {
      nextPresentSec = presentClockSec;
      presentInit = true;
      presentUseWall = useWallClock;
      resetPresentClock = false;
    }
    bool presentDue = (presentClockSec + leadSlack) >= nextPresentSec;
    bool inResumeGrace = now < resumeGraceUntil;

    if (pendingResize) {
      bool resumeAfterResize = false;
      if (paused) {
        decodePaused.store(false);
        queueCv.notify_all();
        resumeAfterResize = true;
      }
      screen.updateSize();
      int width = std::max(20, screen.width());
      int height = std::max(10, screen.height());
      uint64_t epoch = 0;
      if (allowDecoderScale) {
        epoch = requestTargetSize(width, height);
      }
      if (epoch != 0) {
        pendingResizeEpoch = epoch;
        pendingResizeRepause = resumeAfterResize;
      } else if (resumeAfterResize) {
        decodePaused.store(true);
        queueCv.notify_all();
        pendingResizeRepause = false;
      }
      pendingResize = false;
      redraw = true;
    }

    bool pendingApplied = false;
    bool pendingWasActive = (pendingSeekEpoch != 0 || pendingResizeEpoch != 0);
    if (pendingWasActive) {
      uint64_t minEpoch = std::max(pendingSeekEpoch, pendingResizeEpoch);
      QueuedFrame pendingQueued{};
      if (tryPopFrame(pendingQueued, minEpoch)) {
        double prevFrameSec = lastFrameSec;
        setCurrentFrame(pendingQueued.poolIndex);
        frameSec = static_cast<double>(frame->timestamp100ns - firstTs) *
                   ticksToSeconds;
        lastFrameSec = frameSec;
        double durationSec = 0.0;
        if (pendingQueued.info.duration100ns > 0) {
          durationSec =
              static_cast<double>(pendingQueued.info.duration100ns) *
              ticksToSeconds;
        } else if (frameSec > prevFrameSec) {
          durationSec = frameSec - prevFrameSec;
        }
        if (durationSec > 0.0) {
          lastFrameDurationSec = durationSec;
          updateMaxQueue(lastFrameDurationSec);
        }
        videoEnded = false;
        if (pendingSeekAdjustWallClock) {
          auto offsetDuration =
              std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(pendingSeekTargetSec));
          startTime = now - offsetDuration;
          pendingSeekAdjustWallClock = false;
        }
        if (pendingResizeEpoch != 0 &&
            pendingQueued.epoch >= pendingResizeEpoch) {
          decoderTargetW = frame->width;
          decoderTargetH = frame->height;
          perfLogAppendf(&perfLog, "video_scale target=%dx%d actual=%dx%d",
                         requestedTargetW, requestedTargetH, decoderTargetW,
                         decoderTargetH);
          pendingResizeEpoch = 0;
          if (pendingResizeRepause && paused) {
            decodePaused.store(true);
            queueCv.notify_all();
          }
          pendingResizeRepause = false;
        }
        if (pendingSeekEpoch != 0 && pendingQueued.epoch >= pendingSeekEpoch) {
          pendingSeekHoldSec = pendingSeekTargetSec;
          pendingSeekHoldUntil = now + std::chrono::milliseconds(400);
          pendingSeekEpoch = 0;
        }
        resetPresentClock = true;
        resumeGraceUntil = now + std::chrono::milliseconds(500);
        redraw = true;
        forceRefreshArt = true;
        pendingApplied = true;
      }
    }

    QueuedFrame candidate{};
    bool advanced = false;
    int popped = 0;
    int64_t nextTs = 0;
    bool queueEmpty = false;

    if (!pendingApplied && !pendingWasActive && !videoEnded && !paused &&
        !waitingForAudioStart && presentDue) {
      double driftForSkip = haveAudioClock ? (syncSec - lastFrameSec)
                                           : (wallclockElapsed - lastFrameSec);

      if (!inResumeGrace && !decodeEnded.load() && haveAudioClock &&
          driftForSkip > hardResyncThreshold && !commandPendingAtomic.load()) {
        double targetSec = std::max(0.0, syncSec);
        int64_t targetTs =
            firstTs + static_cast<int64_t>(targetSec * secondsToTicks);
        uint64_t skipEstimate = 0;
        if (lastFrameDurationSec > 0.0) {
          double deltaSec = targetSec - lastFrameSec;
          if (deltaSec > 0.0) {
            skipEstimate =
                static_cast<uint64_t>(deltaSec / lastFrameDurationSec);
          }
        }
        uint64_t epoch = commandEpoch.fetch_add(1) + 1;
        DecodeCommand cmd;
        cmd.seek = true;
        cmd.seekTs = targetTs;
        cmd.epoch = epoch;
        issueCommand(cmd);

        size_t droppedFrames = 0;
        {
          std::lock_guard<std::mutex> lock(queueMutex);
          droppedFrames = frameQueue.size();
          while (!frameQueue.empty()) {
            freeFrames.push_back(frameQueue.front().poolIndex);
            frameQueue.pop_front();
          }
        }
        queueCv.notify_all();
        if (droppedFrames > 0) {
          skippedCalls.fetch_add(static_cast<uint64_t>(droppedFrames),
                                 std::memory_order_relaxed);
        }
        if (skipEstimate > droppedFrames) {
          skippedCalls.fetch_add(
              static_cast<uint64_t>(skipEstimate - droppedFrames),
              std::memory_order_relaxed);
        }

        pendingSeekEpoch = epoch;
        pendingSeekTargetSec = targetSec;
        pendingSeekAdjustWallClock = false;
        audioSyncBiasValid = false;
        resumeGraceUntil = now + std::chrono::milliseconds(500);
        resetPresentClock = true;
        redraw = true;
        forceRefreshArt = true;
        perfLogAppendf(&perfLog, "sync_seek_pending audio=%.3f target=%.3f",
                       audioNow, syncSec);
        continue;
      } else {
        if (!inResumeGrace && !haveAudioClock &&
            driftForSkip > hardResyncThreshold) {
          auto offsetDuration =
              std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(lastFrameSec));
          startTime = now - offsetDuration;
          syncSec = std::chrono::duration<double>(now - startTime).count();
          wallclockElapsed = syncSec;
          if (!useWallClock) {
            presentClockSec = syncSec;
          }
          driftForSkip = 0.0;
          perfLogAppendf(&perfLog, "sync_hard_reset wallclock=%.3f video=%.3f",
                         wallclockElapsed, lastFrameSec);
        }

        double targetSec = syncSec + leadSlack;

        bool notifyQueue = false;
        {
          std::lock_guard<std::mutex> lock(queueMutex);
          size_t skippedFrames = 0;

          // If we're significantly behind, skip frames aggressively.
          if (!inResumeGrace && driftForSkip > syncTolerance &&
              !frameQueue.empty()) {
            double targetTime = haveAudioClock ? syncSec : wallclockElapsed;

            while (frameQueue.size() > 1) {
              double frontSec =
                  static_cast<double>(
                      framePool[frameQueue.front().poolIndex].timestamp100ns -
                      firstTs) *
                  ticksToSeconds;
              double nextSec =
                  static_cast<double>(
                      framePool[frameQueue[1].poolIndex].timestamp100ns -
                      firstTs) *
                  ticksToSeconds;

              // If next frame is still behind target, skip current frame.
              if (nextSec <= targetTime) {
                freeFrames.push_back(frameQueue.front().poolIndex);
                frameQueue.pop_front();
                ++popped;
                ++skippedFrames;
              } else {
                break;
              }
            }
          }

          if (skippedFrames > 0) {
            skippedCalls.fetch_add(static_cast<uint64_t>(skippedFrames),
                                   std::memory_order_relaxed);
          }

          // Now take the front frame if it's ready.
          while (!frameQueue.empty()) {
            double qSec =
                static_cast<double>(
                    framePool[frameQueue.front().poolIndex].timestamp100ns -
                    firstTs) *
                ticksToSeconds;

            if (qSec <= targetSec) {
              candidate = std::move(frameQueue.front());
              frameQueue.pop_front();
              ++popped;
              advanced = true;
            } else {
              break;
            }
          }
          if (!frameQueue.empty()) {
            nextTs = frameQueue.front().info.timestamp100ns;
          }
          queueEmpty = frameQueue.empty();
          notifyQueue = popped > 0;

          if (!inResumeGrace && queueEmpty && haveAudioClock &&
              driftForSkip > syncTolerance && !commandPendingAtomic.load() &&
              !decodeEnded.load()) {
            int64_t targetTs =
                firstTs + static_cast<int64_t>(syncSec * secondsToTicks);
            requestFastForward(targetTs);
          }
        }
        if (notifyQueue) {
          queueCv.notify_all();
        }
      }
      double prevFrameSec = lastFrameSec;
      if (advanced) {
        setCurrentFrame(candidate.poolIndex);
        frameSec = static_cast<double>(frame->timestamp100ns - firstTs) *
                   ticksToSeconds;
        lastFrameSec = frameSec;
        double durationSec = 0.0;
        if (candidate.info.duration100ns > 0) {
          durationSec = static_cast<double>(candidate.info.duration100ns) *
                        ticksToSeconds;
        } else if (frameSec > prevFrameSec) {
          durationSec = frameSec - prevFrameSec;
        }
        if (durationSec > 0.0) {
          lastFrameDurationSec = durationSec;
          updateMaxQueue(lastFrameDurationSec);
        }
        if (haveAudioClock) {
          lastSyncDrift = syncSec - lastFrameSec;
        } else {
          lastSyncDrift = wallclockElapsed - lastFrameSec;
        }
      } else if (queueEmpty && decodeEnded.load() &&
                 !(pendingWasActive || pendingApplied)) {
        videoEnded = true;
      }
    }
    if (!videoEnded && decodeEnded.load() &&
        !(pendingWasActive || pendingApplied)) {
      std::lock_guard<std::mutex> lock(queueMutex);
      if (frameQueue.empty()) {
        videoEnded = true;
      }
    }

    if (advanced && presentDue) {
      if (audioHoldActive && audioOk && audioIsPrimed()) {
        audioSetHold(false);
        audioHoldActive = false;
        audioSyncBiasSec = 0.0;
        audioSyncBiasValid = false;
        resetPresentClock = true;
        redraw = true;
      }
      double displayedSec = frameSec;
      int64_t displayedTs = frame->timestamp100ns;
      auto renderStart = std::chrono::steady_clock::now();
      renderScreen(true);
      auto renderEnd = std::chrono::steady_clock::now();
      if (renderFailed) {
        running = false;
        break;
      }
      if (audioOk && !audioSyncBiasValid && audioPrimed) {
        double biasNow = audioGetTimeSec();
        audioSyncBiasSec = biasNow - displayedSec;
        audioSyncBiasValid = std::isfinite(audioSyncBiasSec);
      }
      redraw = false;
      forceRefreshArt = false;

      double renderMs =
          std::chrono::duration<double, std::milli>(renderEnd - renderStart)
              .count();
      double decodeMs = candidate.decodeMs;
      double lagSec = syncSec - displayedSec;
      double syncDriftNow = lastSyncDrift;
      int dropped = popped > 0 ? (popped - 1) : 0;
      uint64_t curReadCalls = readCalls.load();
      uint64_t curSkipped = skippedCalls.load();
      uint64_t curQueueDrops = queueDrops.load();
      int readDelta = static_cast<int>(curReadCalls - lastReadCalls);
      int skippedDelta = static_cast<int>(curSkipped - lastSkippedCalls);
      int queueDropDelta = static_cast<int>(curQueueDrops - lastQueueDrops);
      lastReadCalls = curReadCalls;
      lastSkippedCalls = curSkipped;
      lastQueueDrops = curQueueDrops;
      auto logTime = renderEnd;
      double wallNow =
          std::chrono::duration<double>(logTime - startTime).count();
      double wallDtMs =
          std::chrono::duration<double, std::milli>(logTime - lastLogTime)
              .count();
      lastLogTime = logTime;
      size_t queueSize = 0;
      {
        std::lock_guard<std::mutex> lock(queueMutex);
        queueSize = frameQueue.size();
      }

      AudioPerfStats audioStats{};
      bool haveAudioStats = audioOk;
      if (haveAudioStats) {
        audioStats = audioGetPerfStats();
      }
      if (haveAudioStats) {
        perfLogAppendf(
            &perfLog,
            "frame t=%.3f sync=%.3f lag=%.3f drift=%.3f wall=%.3f "
            "wall_dt_ms=%.2f sleep_ms=%.2f q=%zu disp_ts=%lld next_ts=%lld "
            "render_ms=%.2f decode_ms=%.2f dropped=%d qdrops=%d skipped=%d "
            "read_calls=%d vflags=0x%X vdur=%lld vticks=%d vtype=%d vrec=%u "
            "vhr=0x%08X acb=%llu areq=%llu aread=%llu ashort=%llu "
            "asilence=%llu alast=%u/%u",
            displayedSec, syncSec, lagSec, syncDriftNow, wallNow, wallDtMs,
            lastSleepMs, queueSize, static_cast<long long>(displayedTs),
            static_cast<long long>(nextTs), renderMs, decodeMs, dropped,
            queueDropDelta, skippedDelta, readDelta, candidate.info.flags,
            static_cast<long long>(candidate.info.duration100ns),
            candidate.info.streamTicks, candidate.info.typeChanges,
            candidate.info.recoveries, candidate.info.errorHr,
            static_cast<unsigned long long>(audioStats.callbacks),
            static_cast<unsigned long long>(audioStats.framesRequested),
            static_cast<unsigned long long>(audioStats.framesRead),
            static_cast<unsigned long long>(audioStats.shortReads),
            static_cast<unsigned long long>(audioStats.silentFrames),
            audioStats.lastCallbackFrames, audioStats.lastFramesRead);
      } else {
        perfLogAppendf(
            &perfLog,
            "frame t=%.3f sync=%.3f lag=%.3f drift=%.3f wall=%.3f "
            "wall_dt_ms=%.2f sleep_ms=%.2f q=%zu disp_ts=%lld next_ts=%lld "
            "render_ms=%.2f decode_ms=%.2f dropped=%d qdrops=%d skipped=%d "
            "read_calls=%d vflags=0x%X vdur=%lld vticks=%d vtype=%d vrec=%u "
            "vhr=0x%08X",
            displayedSec, syncSec, lagSec, syncDriftNow, wallNow, wallDtMs,
            lastSleepMs, queueSize, static_cast<long long>(displayedTs),
            static_cast<long long>(nextTs), renderMs, decodeMs, dropped,
            queueDropDelta, skippedDelta, readDelta, candidate.info.flags,
            static_cast<long long>(candidate.info.duration100ns),
            candidate.info.streamTicks, candidate.info.typeChanges,
            candidate.info.recoveries, candidate.info.errorHr);
      }
      nextPresentSec += kPresentIntervalSec;
      if (presentClockSec > nextPresentSec + kPresentIntervalSec) {
        nextPresentSec = presentClockSec;
      }
    }

    if (redraw && !advanced && presentDue) {
      renderScreen(forceRefreshArt);
      if (renderFailed) {
        running = false;
        break;
      }
      redraw = false;
      forceRefreshArt = false;
      nextPresentSec += kPresentIntervalSec;
      if (presentClockSec > nextPresentSec + kPresentIntervalSec) {
        nextPresentSec = presentClockSec;
      }
    }

    int sleepMs = 4;
    if (presentInit) {
      double dt = nextPresentSec - presentClockSec;
      if (dt > 0.0) {
        sleepMs = static_cast<int>(std::clamp(dt * 1000.0, 1.0, 10.0));
      }
    }
    auto sleepStart = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    auto sleepEnd = std::chrono::steady_clock::now();
    lastSleepMs =
        std::chrono::duration<double, std::milli>(sleepEnd - sleepStart)
            .count();
  }

  if (audioStartThread.joinable()) {
    audioStartThread.join();
  }
  if (audioStarting) {
    audioOk = audioStartOk.load();
    audioStarting = false;
  }

  if (renderFailed) {
    stopDecodeThread();
    if (audioOk) audioStop();
    bool ok = reportVideoError(renderFailMessage, renderFailDetail);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  stopDecodeThread();
  if (audioOk) audioStop();
  finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
  return true;
}

