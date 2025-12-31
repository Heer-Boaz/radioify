#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <new>
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

#include "asciiart.h"
#include "consoleinput.h"
#include "consolescreen.h"
#include "m4adecoder.h"
#include "radio.h"
#include "videodecoder.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_ENABLE_WAV
#define MA_ENABLE_MP3
#define MA_ENABLE_FLAC
#include "miniaudio.h"

struct Options {
  std::string input;
  std::string output;
  int bwHz = 4800;
  double noise = 0.012;  // tuned for a "modern recording through 1938 AM"
  // double noise = 0.006; // tuned for a "modern recording through 1938 AM"
  bool mono = true;
  bool play = true;
  bool dry = false;
  bool enableAscii = true;
  bool enableAudio = true;
  bool enableRadio = true;
  bool verbose = false;
};

static void die(const std::string& message) {
  std::cerr << "ERROR: " << message << "\n";
  std::exit(1);
}

static void logLine(const std::string& message) {
  std::cout << message << "\n";
}

static std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static void showUsage(const char* exe) {
  std::string name = exe ? std::string(exe) : "radioify";
  logLine("Usage: " + name + " [options] [file_or_folder]");
  logLine("Options:");
  logLine("  --no-ascii   Disable ASCII video rendering");
  logLine("  --no-audio   Disable audio playback");
  logLine("  --no-radio   Start with radio filter disabled");
  logLine("  -h, --help   Show this help");
}

static Options parseArgs(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      showUsage(argv[0]);
      std::exit(0);
    }
    if (arg == "--no-ascii") {
      o.enableAscii = false;
      continue;
    }
    if (arg == "--no-audio") {
      o.enableAudio = false;
      continue;
    }
    if (arg == "--no-radio") {
      o.enableRadio = false;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      die("Unknown option: " + arg);
    }
    if (!o.input.empty()) {
      die("Provide a single file or folder path only.");
    }
    o.input = arg;
  }
  return o;
}

static std::string toUtf8String(const std::filesystem::path& p) {
#ifdef _WIN32
  auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
#else
  return p.string();
#endif
}

struct PerfLog {
  std::ofstream file;
  std::string buffer;
  bool enabled = false;

  bool open(const std::filesystem::path& path, std::string* error) {
    file.open(path, std::ios::out | std::ios::app);
    if (!file.is_open()) {
      if (error) {
        *error = "Failed to open timing log file: " + toUtf8String(path);
      }
      return false;
    }
    enabled = true;
    return true;
  }

  void appendf(const char* fmt, ...) {
    if (!enabled) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int written = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (written <= 0) return;
    if (written >= static_cast<int>(sizeof(buf))) {
      written = static_cast<int>(sizeof(buf)) - 1;
    }
    buffer.append(buf, buf + written);
    buffer.push_back('\n');
    if (buffer.size() >= 8192) flush();
  }

  void flush() {
    if (!enabled || buffer.empty()) return;
    file << buffer;
    file.flush();
    buffer.clear();
  }

  ~PerfLog() { flush(); }
};

static std::string formatTime(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0) return "--:--";
  int total = static_cast<int>(seconds);
  int hours = total / 3600;
  int minutes = (total % 3600) / 60;
  int secs = total % 60;
  if (hours > 0) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", hours, minutes, secs);
    return buf;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d:%02d", minutes, secs);
  return buf;
}

static float clamp01(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

static Color lerpColor(const Color& a, const Color& b, float t) {
  t = clamp01(t);
  Color out;
  out.r = static_cast<uint8_t>(std::lround(a.r + (b.r - a.r) * t));
  out.g = static_cast<uint8_t>(std::lround(a.g + (b.g - a.g) * t));
  out.b = static_cast<uint8_t>(std::lround(a.b + (b.b - a.b) * t));
  return out;
}

static size_t utf8Next(const std::string& s, size_t i) {
  if (i >= s.size()) return s.size();
  unsigned char c = static_cast<unsigned char>(s[i]);
  if ((c & 0x80) == 0x00) return i + 1;
  if ((c & 0xE0) == 0xC0) return std::min(s.size(), i + 2);
  if ((c & 0xF0) == 0xE0) return std::min(s.size(), i + 3);
  if ((c & 0xF8) == 0xF0) return std::min(s.size(), i + 4);
  return i + 1;
}

static int utf8CodepointCount(const std::string& s) {
  int count = 0;
  for (size_t i = 0; i < s.size();) {
    i = utf8Next(s, i);
    count++;
  }
  return count;
}

static std::string utf8Take(const std::string& s, int count) {
  if (count <= 0) return "";
  size_t i = 0;
  int c = 0;
  for (; i < s.size() && c < count; ++c) {
    i = utf8Next(s, i);
  }
  return s.substr(0, i);
}

static std::string utf8Slice(const std::string& s, int start, int count) {
  if (count <= 0) return "";
  if (start < 0) start = 0;
  size_t i = 0;
  int c = 0;
  for (; i < s.size() && c < start; ++c) {
    i = utf8Next(s, i);
  }
  size_t startByte = i;
  int end = start + count;
  for (; i < s.size() && c < end; ++c) {
    i = utf8Next(s, i);
  }
  if (startByte >= s.size()) return "";
  return s.substr(startByte, i - startByte);
}

static std::string fitLine(const std::string& s, int width) {
  if (width <= 0) return "";
  int count = utf8CodepointCount(s);
  if (count <= width) return s;
  if (width <= 1) return utf8Take(s, width);
  return utf8Take(s, width - 1) + "~";
}

struct BufferCell {
  wchar_t ch = L' ';
  Style style{};
};

struct AudioPerfStats {
  uint64_t callbacks = 0;
  uint64_t framesRequested = 0;
  uint64_t framesRead = 0;
  uint64_t shortReads = 0;
  uint64_t silentFrames = 0;
  uint32_t lastCallbackFrames = 0;
  uint32_t lastFramesRead = 0;
  uint32_t periodFrames = 0;
  uint32_t periods = 0;
  uint32_t bufferFrames = 0;
  uint32_t sampleRate = 0;
  uint32_t channels = 0;
  bool usingM4a = false;
};

struct VideoPlaybackHooks {
  std::function<bool(const std::filesystem::path&)> startAudio;
  std::function<void()> stopAudio;
  std::function<double()> getAudioTimeSec;
  std::function<double()> getAudioTotalSec;
  std::function<bool()> isAudioPaused;
  std::function<bool()> isAudioFinished;
  std::function<AudioPerfStats()> getAudioPerfStats;
  std::function<void()> togglePause;
  std::function<void(int)> seekBy;
  std::function<void(double)> seekToRatio;
  std::function<void()> toggleRadio;
};

static int quantizeCoverage(double value) {
  int quantized = static_cast<int>(std::round(value * 8.0 + 1e-7));
  return std::clamp(quantized, 0, 8);
}

static wchar_t glyphForCoverage(double coverage) {
  static const wchar_t kLeftBlocks[] = {L'\x258F', L'\x258E', L'\x258D',
                                        L'\x258C', L'\x258B', L'\x258A',
                                        L'\x2589', L'\x2588'};
  int idx = quantizeCoverage(coverage);
  if (idx <= 0) return L'\0';
  return kLeftBlocks[idx - 1];
}

static std::vector<BufferCell> renderProgressBarCells(
    double ratio, int barLength, const Style& emptyStyle,
    const Color& fillStart, const Color& fillEnd) {
  std::vector<BufferCell> cells;
  if (barLength <= 0) return cells;
  cells.assign(static_cast<size_t>(barLength),
               BufferCell{L'\x00B7', emptyStyle});
  ratio = std::clamp(ratio, 0.0, 1.0);
  if (ratio <= 0.0) return cells;

  constexpr double kSliverThreshold = 1.0 / 16.0;
  const Color gapColor{0, 0, 0};
  double cellSize = 1.0 / static_cast<double>(barLength);
  double regionStart = 0.0;
  double regionEnd = ratio;

  for (int cell = 0; cell < barLength; ++cell) {
    double cellStart = cell * cellSize;
    double cellEnd = cellStart + cellSize;
    double segStart = std::max(regionStart, cellStart);
    double segEnd = std::min(regionEnd, cellEnd);
    double overlap = segEnd - segStart;
    if (overlap <= 0.0) continue;

    double leftFrac = (segStart - cellStart) / cellSize;
    double rightFrac = (segEnd - cellStart) / cellSize;
    double coverage = rightFrac - leftFrac;
    if (coverage <= 0.0) continue;

    float t = (barLength > 1)
                  ? static_cast<float>(cell) /
                        static_cast<float>(barLength - 1)
                  : 0.0f;
    Color fillColor = lerpColor(fillStart, fillEnd, t);

    if (coverage >= 1.0 - 1e-7) {
      cells[static_cast<size_t>(cell)].ch = L'\x2588';
      cells[static_cast<size_t>(cell)].style =
          Style{fillColor, fillColor};
      continue;
    }

    double leftGap = leftFrac;
    double rightGap = 1.0 - rightFrac;
    bool alignRight = leftGap > 0.0 && (rightGap <= 0.0 || rightGap < leftGap);

    if (alignRight) {
      if (leftGap < kSliverThreshold) {
        cells[static_cast<size_t>(cell)].ch = L'\x2588';
        cells[static_cast<size_t>(cell)].style =
            Style{fillColor, fillColor};
        continue;
      }
      wchar_t glyph = glyphForCoverage(leftGap);
      if (glyph) {
        cells[static_cast<size_t>(cell)].ch = glyph;
        cells[static_cast<size_t>(cell)].style =
            Style{gapColor, fillColor};
      }
    } else {
      if (coverage < kSliverThreshold) continue;
      wchar_t glyph = glyphForCoverage(coverage);
      if (glyph) {
        cells[static_cast<size_t>(cell)].ch = glyph;
        cells[static_cast<size_t>(cell)].style =
            Style{fillColor, emptyStyle.bg};
      }
    }
  }

  return cells;
}

static bool isSupportedImageExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp";
}

static bool isVideoExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".mp4" || ext == ".webm";
}

static bool isSupportedAudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".m4a" ||
         ext == ".webm" || ext == ".mp4";
}

static bool isMiniaudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

static bool isSupportedMediaExt(const std::filesystem::path& p) {
  return isSupportedAudioExt(p) || isSupportedImageExt(p);
}

static bool isM4aExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".m4a" || ext == ".webm" || ext == ".mp4";
}

static void validateInputFile(const std::filesystem::path& p) {
  if (p.empty()) die("Missing input file path.");
  if (!std::filesystem::exists(p)) die("Input file not found: " + p.string());
  if (std::filesystem::is_directory(p))
    die("Input path must be a file: " + p.string());
  if (!isSupportedAudioExt(p)) {
    die("Unsupported input format '" + p.extension().string() +
        "'. Supported: .wav, .mp3, .flac, .m4a, .webm, .mp4.");
  }
}

static std::vector<FileEntry> listEntries(const std::filesystem::path& dir) {
  std::vector<FileEntry> entries;
  std::vector<FileEntry> items;

#ifdef _WIN32
  if (dir.empty() || dir == dir.root_path()) {
    for (const auto& drive : listDriveEntries()) {
      entries.push_back(FileEntry{drive.label, drive.path, true});
    }
  }
#endif

  if (dir.has_parent_path() && dir != dir.root_path()) {
    entries.push_back(FileEntry{"..", dir.parent_path(), true});
  }

  try {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      const auto& p = entry.path();
      if (entry.is_directory()) {
        items.push_back(FileEntry{toUtf8String(p.filename()), p, true});
      } else if (entry.is_regular_file() && isSupportedMediaExt(p)) {
        items.push_back(FileEntry{toUtf8String(p.filename()), p, false});
      }
    }
  } catch (...) {
    return entries;
  }

  std::sort(items.begin(), items.end(),
            [](const FileEntry& a, const FileEntry& b) {
              if (a.isDir != b.isDir) return a.isDir > b.isDir;
              return toLower(a.name) < toLower(b.name);
            });

  entries.insert(entries.end(), items.begin(), items.end());
  return entries;
}

static void refreshBrowser(BrowserState& state,
                           const std::string& initialName) {
  state.entries = listEntries(state.dir);
  if (state.entries.empty()) {
    state.selected = 0;
    state.scrollRow = 0;
    return;
  }

  if (state.selected < 0 ||
      state.selected >= static_cast<int>(state.entries.size())) {
    state.selected = 0;
  }
  state.scrollRow = 0;

  if (!initialName.empty()) {
    for (size_t i = 0; i < state.entries.size(); ++i) {
      if (toLower(state.entries[i].name) == toLower(initialName)) {
        state.selected = static_cast<int>(i);
        break;
      }
    }
  }
}

static std::string fitName(const std::string& name, int colWidth) {
  int maxLen = colWidth - 2;
  if (maxLen <= 1) return name.empty() ? " " : utf8Take(name, 1);
  if (utf8CodepointCount(name) <= maxLen) return name;
  return utf8Take(name, maxLen - 1) + "~";
}

static bool showAsciiArt(const std::filesystem::path& file, ConsoleInput& input,
                         ConsoleScreen& screen, const Style& baseStyle,
                         const Style& accentStyle, const Style& dimStyle) {
  AsciiArt art;
  std::string error;
  bool ok = false;

  auto renderFrame = [&]() {
    screen.updateSize();
    int width = std::max(20, screen.width());
    int height = std::max(10, screen.height());
    const int headerLines = 2;
    const int footerLines = 1;
    int artTop = headerLines;
    int maxHeight = std::max(1, height - headerLines - footerLines);

    error.clear();
    ok = renderAsciiArt(file, width, maxHeight, art, &error);

    screen.clear(baseStyle);
    std::string title = "Preview: " + toUtf8String(file.filename());
    screen.writeText(0, 0, fitLine(title, width), accentStyle);
    screen.writeText(0, 1, fitLine("Press any key to return", width), dimStyle);

    if (!ok) {
      screen.writeText(0, artTop, fitLine("Failed to open image.", width),
                       dimStyle);
      if (!error.empty() && artTop + 1 < height) {
        screen.writeText(0, artTop + 1, fitLine(error, width), dimStyle);
      }
      screen.draw();
      return;
    }

    int artWidth = std::min(art.width, width);
    int artHeight = std::min(art.height, maxHeight);
    int artX = std::max(0, (width - artWidth) / 2);

    for (int y = 0; y < artHeight; ++y) {
      for (int x = 0; x < artWidth; ++x) {
        const auto& cell =
            art.cells[static_cast<size_t>(y * art.width + x)];
        Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
        screen.writeChar(artX + x, artTop + y, cell.ch, cellStyle);
      }
    }
    screen.draw();
  };

  renderFrame();

  InputEvent ev{};
  while (true) {
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Resize) {
        renderFrame();
        continue;
      }
      if (ev.type == InputEvent::Type::Key) {
        return ok;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

static bool showAsciiVideo(
    const std::filesystem::path& file,
    ConsoleInput& input,
    ConsoleScreen& screen,
    const Style& baseStyle,
    const Style& accentStyle,
    const Style& dimStyle,
    const Style& progressEmptyStyle,
    const Style& progressFrameStyle,
    const Color& progressStart,
    const Color& progressEnd,
    bool enableAscii,
    const VideoPlaybackHooks& hooks) {
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
  if (!perfLog.open(logPath, &logError)) {
    return showError("Failed to open timing log file.", logError);
  }
  perfLog.appendf("video_start file=%s", toUtf8String(file.filename()).c_str());

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
  bool running = true;
  bool videoEnded = false;
  bool redraw = true;
  AsciiArt art;
  VideoFrame frame;
  struct QueuedFrame {
    VideoFrame frame;
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
  int decoderTargetW = 0;
  int decoderTargetH = 0;
  int requestedTargetW = 0;
  int requestedTargetH = 0;
  bool pendingResize = false;

  std::mutex queueMutex;
  std::condition_variable queueCv;
  std::deque<QueuedFrame> frameQueue;
  constexpr size_t kMaxQueue = 3;
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
  std::atomic<uint64_t> readCalls{0};
  std::atomic<uint64_t> skippedCalls{0};
  std::atomic<uint64_t> queueDrops{0};
  std::thread decodeThread;
  uint64_t lastReadCalls = 0;
  uint64_t lastQueueDrops = 0;
  uint64_t lastSkippedCalls = 0;
  double audioSyncOffset = 0.0;
  bool audioSyncInit = false;

  auto clearQueue = [&]() {
    std::lock_guard<std::mutex> lock(queueMutex);
    frameQueue.clear();
  };

  auto setDecodeError = [&](const std::string& message) {
    std::lock_guard<std::mutex> lock(decodeErrorMutex);
    decodeError = message;
    decodeFailed.store(true);
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

  auto waitForFrame = [&](QueuedFrame& out, uint64_t minEpoch) -> bool {
    std::unique_lock<std::mutex> lock(queueMutex);
    while (true) {
      queueCv.wait(lock, [&]() {
        return !frameQueue.empty() || decodeEnded.load();
      });
      while (!frameQueue.empty()) {
        QueuedFrame candidate = std::move(frameQueue.front());
        frameQueue.pop_front();
        if (candidate.epoch >= minEpoch) {
          out = std::move(candidate);
          return true;
        }
      }
      if (decodeEnded.load()) {
        return false;
      }
    }
  };

  auto computeTargetSize = [&](int width, int height) {
    const int headerLines = 2;
    const int footerLines = 2;
    int maxHeight = std::max(1, height - headerLines - footerLines);
    int maxOutW = std::max(1, width - 8);
    int srcW = std::max(1, sourceWidth);
    int srcH = std::max(1, sourceHeight);
    int outW = std::max(1, std::min(maxOutW, srcW / 2));
    int outH = static_cast<int>(
        std::lround(outW * (static_cast<float>(srcH) / srcW) / 2.0f));
    outH = std::max(1, std::min(outH, srcH / 4));
    if (maxHeight > 0) outH = std::min(outH, maxHeight);
    int targetW = std::max(2, outW * 2);
    int targetH = std::max(4, outH * 4);
    if (targetW & 1) ++targetW;
    if (targetH & 1) ++targetH;
    if (sourceWidth > 0) targetW = std::min(targetW, sourceWidth & ~1);
    if (sourceHeight > 0) targetH = std::min(targetH, sourceHeight & ~1);
    targetW = std::max(2, targetW);
    targetH = std::max(4, targetH);
    return std::pair<int, int>(targetW, targetH);
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
      std::string error;
      if (!decoder.init(file, &error)) {
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
      {
        std::lock_guard<std::mutex> lock(initMutex);
        initDone = true;
        initOk = true;
        initError.clear();
        sourceWidth = decoder.width();
        sourceHeight = decoder.height();
        sourceDuration100ns = decoder.duration100ns();
      }
      initCv.notify_all();

      uint64_t currentEpoch = 0;
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
          decoder.flush();
          if (command.resize) {
            std::string sizeError;
            if (!decoder.setTargetSize(command.targetW, command.targetH,
                                       &sizeError)) {
              // Keep decoding at the current size if scaling isn't supported.
            }
          }
          if (command.seek) {
            if (!decoder.seekToTimestamp100ns(command.seekTs)) {
              setDecodeError("Failed to seek video.");
              decodeEnded.store(true);
              break;
            }
          }
          currentEpoch = command.epoch;
          clearQueue();
        }
        if (decodePaused.load()) {
          std::unique_lock<std::mutex> lock(queueMutex);
          queueCv.wait_for(lock, std::chrono::milliseconds(30), [&]() {
            return !decodeRunning.load() || !decodePaused.load() ||
                   commandPendingAtomic.load();
          });
          continue;
        }

        bool queueFull = false;
        {
          std::lock_guard<std::mutex> lock(queueMutex);
          if (frameQueue.size() >= kMaxQueue) {
            queueFull = true;
          }
        }
        if (queueFull) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }

        VideoFrame decodedFrame;
        VideoReadInfo info{};
        auto decodeStart = std::chrono::steady_clock::now();
        bool ok = decoder.readFrame(decodedFrame, &info, enableAscii);
        auto decodeEnd = std::chrono::steady_clock::now();
        if (!ok) {
          if (!decoder.atEnd()) {
            setDecodeError("Failed to decode video frame.");
          }
          decodeEnded.store(true);
          break;
        }
        readCalls.fetch_add(1, std::memory_order_relaxed);

        double decodeMs =
            std::chrono::duration<double, std::milli>(decodeEnd - decodeStart)
                .count();
        {
          std::lock_guard<std::mutex> lock(queueMutex);
          if (frameQueue.size() >= kMaxQueue) {
            frameQueue.pop_front();
            queueDrops.fetch_add(1, std::memory_order_relaxed);
          }
          frameQueue.push_back(
              QueuedFrame{std::move(decodedFrame), info, decodeMs, currentEpoch});
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

  startDecodeThread();
  {
    std::unique_lock<std::mutex> lock(initMutex);
    initCv.wait(lock, [&]() { return initDone; });
  }
  if (!initOk) {
    stopDecodeThread();
    if (initError.rfind("No video stream found", 0) == 0) {
      if (!hooks.startAudio) {
        return showError("No video stream found.",
                         "Audio playback is disabled.");
      }
      bool playAudio = showAudioFallbackPrompt(
          "No video stream found.",
          "This file can be played as audio only.");
      return playAudio ? false : true;
    }
    if (initError.empty()) {
      initError = "Failed to open video.";
    }
    return showError(initError, "");
  }

  audioOk = hooks.startAudio ? hooks.startAudio(file) : false;
  perfLog.appendf("audio_start ok=%d", audioOk ? 1 : 0);
  if (audioOk && hooks.getAudioPerfStats) {
    AudioPerfStats stats = hooks.getAudioPerfStats();
    if (stats.periodFrames > 0 && stats.periods > 0) {
      perfLog.appendf(
          "audio_device period_frames=%u periods=%u buffer_frames=%u rate=%u channels=%u using_m4a=%d",
          stats.periodFrames, stats.periods, stats.bufferFrames,
          stats.sampleRate, stats.channels, stats.usingM4a ? 1 : 0);
    }
  }

  VideoReadInfo firstInfo{};
  const bool allowDecoderScale = false;
  screen.updateSize();
  int initWidth = std::max(20, screen.width());
  int initHeight = std::max(10, screen.height());
  uint64_t pendingScaleEpoch = 0;
  if (allowDecoderScale) {
    pendingScaleEpoch = requestTargetSize(initWidth, initHeight);
  }

  QueuedFrame firstQueued{};
  if (!waitForFrame(firstQueued, pendingScaleEpoch)) {
    stopDecodeThread();
    if (audioOk && hooks.stopAudio) hooks.stopAudio();
    std::string fail = getDecodeError();
    if (fail.empty()) {
      fail = "No video frames found.";
    }
    return showError(fail, "");
  }
  frame = std::move(firstQueued.frame);
  firstInfo = firstQueued.info;
  decoderTargetW = frame.width;
  decoderTargetH = frame.height;
  if (pendingScaleEpoch != 0) {
    perfLog.appendf("video_scale target=%dx%d actual=%dx%d", requestedTargetW,
                    requestedTargetH, decoderTargetW, decoderTargetH);
  } else {
    requestedTargetW = decoderTargetW;
    requestedTargetH = decoderTargetH;
  }
  perfLog.appendf(
      "first_frame ts100ns=%lld dur100ns=%lld flags=0x%X rec=%u ehr=0x%08X size=%dx%d",
      static_cast<long long>(frame.timestamp100ns),
      static_cast<long long>(firstInfo.duration100ns), firstInfo.flags,
      firstInfo.recoveries, firstInfo.errorHr, frame.width, frame.height);

  const double ticksToSeconds = 1.0 / 10000000.0;
  const double secondsToTicks = 10000000.0;
  int64_t firstTs = frame.timestamp100ns;
  double frameSec =
      static_cast<double>(frame.timestamp100ns - firstTs) * ticksToSeconds;
  double lastFrameSec = frameSec;
  if (audioOk && hooks.getAudioTimeSec) {
    double audioNow = hooks.getAudioTimeSec();
    audioSyncOffset = audioNow - frameSec;
    audioSyncInit = true;
  }
  auto startTime = std::chrono::steady_clock::now();
  auto lastUiUpdate = startTime;

  auto totalDurationSec = [&]() -> double {
    double total = sourceDuration100ns > 0
                       ? sourceDuration100ns * ticksToSeconds
                       : -1.0;
    if (total <= 0.0 && audioOk && hooks.getAudioTotalSec) {
      total = hooks.getAudioTotalSec();
    }
    return total;
  };

  auto currentTimeSec = [&]() -> double {
    if (audioOk && hooks.getAudioTimeSec) {
      double audioNow = hooks.getAudioTimeSec();
      if (!audioSyncInit) {
        audioSyncOffset = audioNow - lastFrameSec;
        audioSyncInit = true;
      }
      return std::max(0.0, audioNow - audioSyncOffset);
    }
    return lastFrameSec;
  };

  auto renderScreen = [&](bool refreshArt) {
    screen.updateSize();
    int width = std::max(20, screen.width());
    int height = std::max(10, screen.height());
    const int headerLines = 2;
    const int footerLines = 2;
    int artTop = headerLines;
    int maxHeight = std::max(1, height - headerLines - footerLines);

    bool sizeChanged =
        (width != cachedWidth || maxHeight != cachedMaxHeight ||
         frame.width != cachedFrameWidth || frame.height != cachedFrameHeight);
    if (enableAscii) {
      if (refreshArt || sizeChanged) {
        if (frame.width <= 0 || frame.height <= 0) {
          renderFailed = true;
          renderFailMessage = "Invalid video frame size.";
          renderFailDetail = "";
          return;
        }
        size_t expected = static_cast<size_t>(frame.width) *
                          static_cast<size_t>(frame.height) * 4u;
        if (frame.rgba.size() < expected) {
          renderFailed = true;
          renderFailMessage = "Invalid video frame buffer.";
          renderFailDetail = "Frame pixel data is missing.";
          return;
        }
        bool artOk = false;
        try {
          artOk = renderAsciiArtFromRgba(frame.rgba.data(), frame.width,
                                         frame.height, width, maxHeight, art,
                                         true);
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
        cachedFrameWidth = frame.width;
        cachedFrameHeight = frame.height;
      }
    } else {
      if (frame.width <= 0 || frame.height <= 0) {
        renderFailed = true;
        renderFailMessage = "Invalid video frame size.";
        renderFailDetail = "";
        return;
      }
      cachedWidth = width;
      cachedMaxHeight = maxHeight;
      cachedFrameWidth = frame.width;
      cachedFrameHeight = frame.height;
    }

    screen.clear(baseStyle);
    std::string title = "Video: " + toUtf8String(file.filename());
    screen.writeText(0, 0, fitLine(title, width), accentStyle);
    std::string subtitle;
    if (!audioOk) {
      subtitle = hooks.startAudio ? "Audio unavailable" : "Audio disabled";
    }
    if (!subtitle.empty()) {
      screen.writeText(0, 1, fitLine(subtitle, width), dimStyle);
    }

    if (enableAscii) {
      int artWidth = std::min(art.width, width);
      int artHeight = std::min(art.height, maxHeight);
      int artX = std::max(0, (width - artWidth) / 2);

      for (int y = 0; y < artHeight; ++y) {
        for (int x = 0; x < artWidth; ++x) {
          const auto& cell =
              art.cells[static_cast<size_t>(y * art.width + x)];
          Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
          screen.writeChar(artX + x, artTop + y, cell.ch, cellStyle);
        }
      }
    } else {
      std::string label = "ASCII rendering disabled";
      screen.writeText(0, artTop, fitLine(label, width), dimStyle);
      if (maxHeight > 1) {
        std::string sizeLine = "Video size: " +
                               std::to_string(frame.width) + "x" +
                               std::to_string(frame.height);
        screen.writeText(0, artTop + 1, fitLine(sizeLine, width), dimStyle);
      }
    }

    int footerLine = artTop + maxHeight;
    std::string nowLabel = " " + toUtf8String(file.filename());
    screen.writeText(0, footerLine++, fitLine(nowLabel, width), accentStyle);

    double currentSec = currentTimeSec();
    double totalSec = totalDurationSec();
    if (totalSec > 0.0) {
      currentSec = std::clamp(currentSec, 0.0, totalSec);
    }
    std::string status;
    if (audioOk && hooks.isAudioFinished && hooks.isAudioFinished()) {
      status = "\xE2\x96\xA0";  // ended icon
    } else if (audioOk && hooks.isAudioPaused && hooks.isAudioPaused()) {
      status = "\xE2\x8F\xB8";  // paused icon
    } else {
      status = "\xE2\x96\xB6";  // playing icon
    }
    std::string suffix =
        formatTime(currentSec) + " / " + formatTime(totalSec) + " " + status;
    int suffixWidth = utf8CodepointCount(suffix);
    int barWidth = width - suffixWidth - 3;
    if (barWidth < 10) {
      suffix = formatTime(currentSec) + "/" + formatTime(totalSec);
      suffixWidth = utf8CodepointCount(suffix);
      barWidth = width - suffixWidth - 3;
    }
    if (barWidth < 10) {
      suffix = formatTime(currentSec);
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
      ratio = std::clamp(currentSec / totalSec, 0.0, 1.0);
    }
    progressBarX = 1;
    progressBarY = footerLine;
    progressBarWidth = barWidth;
    screen.writeChar(0, footerLine, L'|', progressFrameStyle);
    auto barCells = renderProgressBarCells(ratio, barWidth,
                                           progressEmptyStyle, progressStart,
                                           progressEnd);
    for (int i = 0; i < barWidth; ++i) {
      const auto& cell = barCells[static_cast<size_t>(i)];
      screen.writeChar(1 + i, footerLine, cell.ch, cell.style);
    }
    screen.writeChar(1 + barWidth, footerLine, L'|', progressFrameStyle);
    if (!suffix.empty()) {
      screen.writeText(2 + barWidth, footerLine, " " + suffix, baseStyle);
    }

    screen.draw();
  };

  renderScreen(true);
  if (renderFailed) {
    stopDecodeThread();
    if (hooks.stopAudio) hooks.stopAudio();
    return showError(renderFailMessage, renderFailDetail);
  }

  while (running) {
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
          if (hooks.togglePause) hooks.togglePause();
          redraw = true;
        } else if (key.vk == 'R' || key.ch == 'r' || key.ch == 'R') {
          if (hooks.toggleRadio) hooks.toggleRadio();
          redraw = true;
        } else if (ctrl && (key.vk == VK_LEFT || key.vk == VK_RIGHT)) {
          int dir = (key.vk == VK_LEFT) ? -1 : 1;
          double currentSec = currentTimeSec();
          double totalSec = totalDurationSec();
          double targetSec = currentSec + dir * 5.0;
          if (totalSec > 0.0) {
            targetSec = std::clamp(targetSec, 0.0, totalSec);
          } else if (targetSec < 0.0) {
            targetSec = 0.0;
          }
          if (hooks.seekBy) hooks.seekBy(dir);
          int64_t targetTs =
              firstTs + static_cast<int64_t>(targetSec * secondsToTicks);
          uint64_t epoch = commandEpoch.fetch_add(1) + 1;
          DecodeCommand cmd;
          cmd.seek = true;
          cmd.seekTs = targetTs;
          cmd.epoch = epoch;
          issueCommand(cmd);
          QueuedFrame seekQueued{};
          if (!waitForFrame(seekQueued, epoch)) {
            stopDecodeThread();
            if (audioOk && hooks.stopAudio) hooks.stopAudio();
            std::string fail = getDecodeError();
            if (fail.empty()) {
              fail = "Failed to seek video.";
            }
            return showError(fail, "");
          }
          frame = std::move(seekQueued.frame);
          frameSec = static_cast<double>(frame.timestamp100ns - firstTs) *
                     ticksToSeconds;
          lastFrameSec = frameSec;
          videoEnded = false;
          if (audioOk && hooks.getAudioTimeSec) {
            double audioNow = hooks.getAudioTimeSec();
            audioSyncOffset = audioNow - frameSec;
            audioSyncInit = true;
          }
          redraw = true;
          if (!audioOk) {
            startTime = std::chrono::steady_clock::now() -
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(targetSec));
          }
        }
      }
      if (ev.type == InputEvent::Type::Mouse) {
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
                if (audioOk && hooks.seekToRatio) {
                  hooks.seekToRatio(ratio);
                }
                double targetSec = ratio * totalSec;
                int64_t targetTs =
                    firstTs +
                    static_cast<int64_t>(targetSec * secondsToTicks);
                uint64_t epoch = commandEpoch.fetch_add(1) + 1;
                DecodeCommand cmd;
                cmd.seek = true;
                cmd.seekTs = targetTs;
                cmd.epoch = epoch;
                issueCommand(cmd);
                QueuedFrame seekQueued{};
                if (!waitForFrame(seekQueued, epoch)) {
                  stopDecodeThread();
                  if (audioOk && hooks.stopAudio) hooks.stopAudio();
                  std::string fail = getDecodeError();
                  if (fail.empty()) {
                    fail = "Failed to seek video.";
                  }
                  return showError(fail, "");
                }
                frame = std::move(seekQueued.frame);
                frameSec =
                    static_cast<double>(frame.timestamp100ns - firstTs) *
                    ticksToSeconds;
                lastFrameSec = frameSec;
                videoEnded = false;
                if (audioOk && hooks.getAudioTimeSec) {
                  double audioNow = hooks.getAudioTimeSec();
                  audioSyncOffset = audioNow - frameSec;
                  audioSyncInit = true;
                }
                if (!audioOk) {
                  startTime = std::chrono::steady_clock::now() -
                              std::chrono::duration_cast<
                                  std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(targetSec));
                }
                redraw = true;
              }
              continue;
            }
          }
        }
      }
    }
    if (!running) break;

    auto now = std::chrono::steady_clock::now();
    if (now - lastUiUpdate >= std::chrono::milliseconds(150)) {
      redraw = true;
      lastUiUpdate = now;
    }

    bool paused = audioOk && hooks.isAudioPaused && hooks.isAudioPaused();
    decodePaused.store(paused);
    queueCv.notify_all();
    double syncSec = 0.0;
    if (audioOk && hooks.getAudioTimeSec) {
      double audioNow = hooks.getAudioTimeSec();
      if (!audioSyncInit) {
        audioSyncOffset = audioNow - lastFrameSec;
        audioSyncInit = true;
      }
      syncSec = std::max(0.0, audioNow - audioSyncOffset);
    } else {
      syncSec = std::chrono::duration<double>(now - startTime).count();
    }
    const double leadSlack = 0.005;

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
        QueuedFrame resizeQueued{};
        if (!waitForFrame(resizeQueued, epoch)) {
          stopDecodeThread();
          if (audioOk && hooks.stopAudio) hooks.stopAudio();
          std::string fail = getDecodeError();
          if (fail.empty()) {
            fail = "Failed to read video frame after resize.";
          }
          return showError(fail, "");
        }
        frame = std::move(resizeQueued.frame);
        frameSec =
            static_cast<double>(frame.timestamp100ns - firstTs) *
            ticksToSeconds;
        lastFrameSec = frameSec;
        videoEnded = false;
        decoderTargetW = frame.width;
        decoderTargetH = frame.height;
        perfLog.appendf("video_scale target=%dx%d actual=%dx%d",
                        requestedTargetW, requestedTargetH, decoderTargetW,
                        decoderTargetH);
        if (audioOk && hooks.getAudioTimeSec) {
          double audioNow = hooks.getAudioTimeSec();
          audioSyncOffset = audioNow - frameSec;
          audioSyncInit = true;
        }
      }
      if (resumeAfterResize) {
        decodePaused.store(true);
        queueCv.notify_all();
      }
      decodePaused.store(paused);
      queueCv.notify_all();
      pendingResize = false;
      redraw = true;
    }

    QueuedFrame candidate{};
    bool advanced = false;
    int popped = 0;
    int64_t nextTs = 0;
    bool queueEmpty = false;

    if (!videoEnded && !paused) {
      double targetSec = syncSec + leadSlack;
      {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!frameQueue.empty()) {
          double qSec =
              static_cast<double>(frameQueue.front().frame.timestamp100ns -
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
      }
      if (advanced) {
        frame = std::move(candidate.frame);
        frameSec =
            static_cast<double>(frame.timestamp100ns - firstTs) * ticksToSeconds;
        lastFrameSec = frameSec;
      } else if (queueEmpty && decodeEnded.load()) {
        videoEnded = true;
      }
    }

    if (advanced) {
      double displayedSec = frameSec;
      int64_t displayedTs = frame.timestamp100ns;
      auto renderStart = std::chrono::steady_clock::now();
      renderScreen(true);
      auto renderEnd = std::chrono::steady_clock::now();
      if (renderFailed) {
        running = false;
        break;
      }
      redraw = false;

      double renderMs =
          std::chrono::duration<double, std::milli>(renderEnd - renderStart)
              .count();
      double decodeMs = candidate.decodeMs;
      double lagSec = syncSec - displayedSec;
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

      AudioPerfStats audioStats{};
      bool haveAudioStats = audioOk && hooks.getAudioPerfStats;
      if (haveAudioStats) {
        audioStats = hooks.getAudioPerfStats();
      }
      if (haveAudioStats) {
        perfLog.appendf(
            "frame t=%.3f sync=%.3f lag=%.3f disp_ts=%lld next_ts=%lld render_ms=%.2f decode_ms=%.2f dropped=%d qdrops=%d skipped=%d read_calls=%d vflags=0x%X vdur=%lld vticks=%d vtype=%d vrec=%u vhr=0x%08X acb=%llu areq=%llu aread=%llu ashort=%llu asilence=%llu alast=%u/%u",
            displayedSec, syncSec, lagSec,
            static_cast<long long>(displayedTs),
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
        perfLog.appendf(
            "frame t=%.3f sync=%.3f lag=%.3f disp_ts=%lld next_ts=%lld render_ms=%.2f decode_ms=%.2f dropped=%d qdrops=%d skipped=%d read_calls=%d vflags=0x%X vdur=%lld vticks=%d vtype=%d vrec=%u vhr=0x%08X",
            displayedSec, syncSec, lagSec,
            static_cast<long long>(displayedTs),
            static_cast<long long>(nextTs), renderMs, decodeMs, dropped,
            queueDropDelta, skippedDelta, readDelta, candidate.info.flags,
            static_cast<long long>(candidate.info.duration100ns),
            candidate.info.streamTicks, candidate.info.typeChanges,
            candidate.info.recoveries, candidate.info.errorHr);
      }
    }

    if (redraw && !advanced) {
      renderScreen(false);
      if (renderFailed) {
        running = false;
        break;
      }
      redraw = false;
    }

    bool audioFinished =
        audioOk && hooks.isAudioFinished && hooks.isAudioFinished();
    if (!audioOk && videoEnded) break;
    if (audioOk && audioFinished) break;

    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }

  if (renderFailed) {
    if (hooks.stopAudio) hooks.stopAudio();
    return showError(renderFailMessage, renderFailDetail);
  }

  stopDecodeThread();
  if (hooks.stopAudio) hooks.stopAudio();
  return true;
}

static GridLayout buildLayout(const BrowserState& state, int width,
                              int rowsVisible) {
  GridLayout layout;
  layout.names.reserve(state.entries.size());
  int maxName = 0;
  for (const auto& e : state.entries) {
    std::string name = e.name;
    if (e.isDir && name != "..") name += "/";
    layout.names.push_back(name);
    maxName = std::max(maxName, utf8CodepointCount(name));
  }

  int safeRows = std::max(1, rowsVisible);
  int colWidth = std::min(width, std::max(8, maxName + 3));
  int maxCols = std::max(1, width / colWidth);
  int cols = std::max(
      1,
      std::min(maxCols, static_cast<int>((state.entries.size() + safeRows - 1) /
                                         safeRows)));
  int totalRows =
      cols > 0 ? static_cast<int>((state.entries.size() + cols - 1) / cols) : 0;
  int visibleRows = std::min(safeRows, totalRows);

  layout.rowsVisible = visibleRows;
  layout.totalRows = totalRows;
  layout.cols = cols;
  layout.colWidth = colWidth;
  return layout;
}

struct AudioRingBuffer {
  std::vector<float> data;
  size_t capacityFrames = 0;
  size_t readPos = 0;
  size_t writePos = 0;
  size_t bufferedFrames = 0;

  void init(size_t frames, uint32_t channels) {
    capacityFrames = frames;
    data.assign(frames * channels, 0.0f);
    readPos = 0;
    writePos = 0;
    bufferedFrames = 0;
  }

  void reset() {
    readPos = 0;
    writePos = 0;
    bufferedFrames = 0;
  }

  size_t space() const {
    if (capacityFrames < bufferedFrames) return 0;
    return capacityFrames - bufferedFrames;
  }

  size_t read(float* out, size_t frames, uint32_t channels) {
    if (!out || capacityFrames == 0 || frames == 0) return 0;
    size_t toRead = std::min(frames, bufferedFrames);
    if (toRead == 0) return 0;
    size_t first = std::min(toRead, capacityFrames - readPos);
    size_t firstSamples = first * channels;
    std::memcpy(out, data.data() + readPos * channels,
                firstSamples * sizeof(float));
    if (toRead > first) {
      size_t second = toRead - first;
      std::memcpy(out + firstSamples, data.data(),
                  second * channels * sizeof(float));
    }
    readPos = (readPos + toRead) % capacityFrames;
    bufferedFrames -= toRead;
    return toRead;
  }

  size_t write(const float* in, size_t frames, uint32_t channels) {
    if (!in || capacityFrames == 0 || frames == 0) return 0;
    size_t spaceFrames = space();
    size_t toWrite = std::min(frames, spaceFrames);
    if (toWrite == 0) return 0;
    size_t first = std::min(toWrite, capacityFrames - writePos);
    size_t firstSamples = first * channels;
    std::memcpy(data.data() + writePos * channels, in,
                firstSamples * sizeof(float));
    if (toWrite > first) {
      size_t second = toWrite - first;
      std::memcpy(data.data(), in + firstSamples,
                  second * channels * sizeof(float));
    }
    writePos = (writePos + toWrite) % capacityFrames;
    bufferedFrames += toWrite;
    return toWrite;
  }
};

struct AudioState {
  ma_decoder decoder{};
  ma_device device{};
  Radio1938 radio1938{};
  AudioRingBuffer m4aBuffer;
  std::mutex m4aMutex;
  std::condition_variable m4aCv;
  std::thread m4aThread;
  bool m4aInitDone = false;
  bool m4aInitOk = false;
  std::string m4aInitError;
  std::atomic<bool> m4aThreadRunning{false};
  std::atomic<bool> m4aStop{false};
  std::atomic<bool> m4aAtEnd{false};
  std::atomic<bool> paused{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> useRadio1938{true};
  std::atomic<bool> usingM4a{false};
  std::atomic<bool> seekRequested{false};
  std::atomic<int64_t> pendingSeekFrames{0};
  std::atomic<uint64_t> framesPlayed{0};
  std::atomic<uint64_t> callbackCount{0};
  std::atomic<uint64_t> framesRequested{0};
  std::atomic<uint64_t> framesReadTotal{0};
  std::atomic<uint64_t> shortReadCount{0};
  std::atomic<uint64_t> silentFrames{0};
  std::atomic<uint64_t> pausedCallbacks{0};
  std::atomic<uint32_t> lastCallbackFrames{0};
  std::atomic<uint32_t> lastFramesRead{0};
  std::atomic<uint64_t> audioClockFrames{0};
  std::atomic<uint64_t> totalFrames{0};
  uint32_t channels = 1;
  uint32_t sampleRate = 48000;
  bool dry = false;
};

static void dataCallback(ma_device* device, void* output, const void*,
                         ma_uint32 frameCount) {
  auto* state = static_cast<AudioState*>(device->pUserData);
  float* out = static_cast<float*>(output);
  if (!state) return;

  state->callbackCount.fetch_add(1, std::memory_order_relaxed);
  state->framesRequested.fetch_add(frameCount, std::memory_order_relaxed);
  state->lastCallbackFrames.store(frameCount, std::memory_order_relaxed);

  bool usingM4a = state->usingM4a.load();
  if (!usingM4a && state->seekRequested.exchange(false)) {
    int64_t target = state->pendingSeekFrames.load();
    if (target < 0) target = 0;
    ma_decoder_seek_to_pcm_frame(&state->decoder,
                                 static_cast<ma_uint64>(target));
    state->framesPlayed.store(static_cast<uint64_t>(target));
    state->audioClockFrames.store(static_cast<uint64_t>(target));
    state->finished.store(false);
  }

  if (state->paused.load()) {
    state->pausedCallbacks.fetch_add(1, std::memory_order_relaxed);
    state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
    state->lastFramesRead.store(0, std::memory_order_relaxed);
    std::fill(out, out + frameCount * state->channels, 0.0f);
    return;
  }

  uint64_t framesRead = 0;
  if (usingM4a) {
    if (state->seekRequested.load()) {
      std::lock_guard<std::mutex> lock(state->m4aMutex);
      state->m4aBuffer.reset();
      state->m4aAtEnd.store(false);
      state->finished.store(false);
    }
    size_t readFrames = 0;
    {
      std::lock_guard<std::mutex> lock(state->m4aMutex);
      readFrames = state->m4aBuffer.read(out, frameCount, state->channels);
    }
    framesRead = readFrames;
    if (readFrames < frameCount) {
      std::fill(out + readFrames * state->channels,
                out + frameCount * state->channels, 0.0f);
    }
    if (readFrames == 0 && state->m4aAtEnd.load()) {
      state->finished.store(true);
      uint64_t total = state->totalFrames.load();
      if (total > 0) {
        state->framesPlayed.store(total);
        state->audioClockFrames.store(total);
      }
    }
    state->m4aCv.notify_one();
  } else {
    ma_uint64 framesReadMa = 0;
    ma_result res = ma_decoder_read_pcm_frames(&state->decoder, out, frameCount,
                                               &framesReadMa);
    if (res != MA_SUCCESS && res != MA_AT_END) {
      state->finished.store(true);
      return;
    }
    framesRead = framesReadMa;
    if (framesRead < frameCount) {
      std::fill(out + framesRead * state->channels,
                out + frameCount * state->channels, 0.0f);
    }
    if (res == MA_AT_END || framesRead == 0) {
      state->finished.store(true);
      uint64_t total = state->totalFrames.load();
      if (total > 0) {
        state->framesPlayed.store(total);
        state->audioClockFrames.store(total);
      }
    }
  }

  state->audioClockFrames.fetch_add(frameCount, std::memory_order_relaxed);
  if (!state->dry && framesRead > 0 && state->useRadio1938.load()) {
    state->radio1938.process(out, static_cast<uint32_t>(framesRead));
  }
  state->framesPlayed.fetch_add(framesRead);
  state->framesReadTotal.fetch_add(framesRead, std::memory_order_relaxed);
  state->lastFramesRead.store(static_cast<uint32_t>(framesRead),
                              std::memory_order_relaxed);
  if (framesRead < frameCount) {
    state->shortReadCount.fetch_add(1, std::memory_order_relaxed);
    state->silentFrames.fetch_add(frameCount - framesRead,
                                  std::memory_order_relaxed);
  }
}

static void renderToFile(const Options& o, const Radio1938& radio1938Template,
                         bool useRadio1938) {
  const uint32_t sampleRate = 48000;
  const uint32_t channels = useRadio1938 ? 1 : (o.mono ? 1 : 2);
  const bool useM4a = isM4aExt(o.input);
  ma_decoder decoder{};
  M4aDecoder m4a{};
  if (useM4a) {
    std::string error;
    if (!m4a.init(o.input, channels, sampleRate, &error)) {
      die(error.empty() ? "Failed to open input for decoding." : error);
    }
  } else {
    ma_decoder_config decConfig =
        ma_decoder_config_init(ma_format_f32, channels, sampleRate);
    if (ma_decoder_init_file(o.input.c_str(), &decConfig, &decoder) !=
        MA_SUCCESS) {
      die("Failed to open input for decoding.");
    }
  }

  ma_encoder encoder{};
  ma_encoder_config encConfig = ma_encoder_config_init(
      ma_encoding_format_wav, ma_format_s16, channels, sampleRate);
  if (ma_encoder_init_file(o.output.c_str(), &encConfig, &encoder) !=
      MA_SUCCESS) {
    if (useM4a) {
      m4a.uninit();
    } else {
      ma_decoder_uninit(&decoder);
    }
    die("Failed to open output for encoding.");
  }

  Radio1938 radio1938 = radio1938Template;
  radio1938.init(channels, static_cast<float>(sampleRate),
                 static_cast<float>(o.bwHz), static_cast<float>(o.noise));
  constexpr uint32_t chunkFrames = 1024;
  std::vector<float> buffer(chunkFrames * channels);
  std::vector<int16_t> out(buffer.size());

  while (true) {
    uint64_t framesRead = 0;
    if (useM4a) {
      if (!m4a.readFrames(buffer.data(), chunkFrames, &framesRead)) {
        ma_encoder_uninit(&encoder);
        m4a.uninit();
        die("Failed to decode input.");
      }
      if (framesRead == 0) break;
    } else {
      ma_uint64 framesReadMa = 0;
      ma_result res = ma_decoder_read_pcm_frames(&decoder, buffer.data(),
                                                 chunkFrames, &framesReadMa);
      framesRead = framesReadMa;
      if (framesRead == 0 || res == MA_AT_END) break;
    }

    if (!o.dry && useRadio1938) {
      radio1938.process(buffer.data(), static_cast<uint32_t>(framesRead));
    }

    for (size_t i = 0; i < static_cast<size_t>(framesRead * channels); ++i) {
      float v = std::clamp(buffer[i], -1.0f, 1.0f);
      out[i] = static_cast<int16_t>(std::lrint(v * 32767.0f));
    }

    ma_uint64 framesWritten = 0;
    ma_encoder_write_pcm_frames(&encoder, out.data(),
                                static_cast<ma_uint64>(framesRead),
                                &framesWritten);
  }

  ma_encoder_uninit(&encoder);
  if (useM4a) {
    m4a.uninit();
  } else {
    ma_decoder_uninit(&decoder);
  }
}

int main(int argc, char** argv) {
  Options o = parseArgs(argc, argv);

  float lpHz = static_cast<float>(o.bwHz);
  const uint32_t sampleRate = 48000;
  const uint32_t baseChannels = o.mono ? 1 : 2;
  uint32_t channels = baseChannels;

  Radio1938 radio1938Template;
  radio1938Template.init(channels, static_cast<float>(sampleRate), lpHz,
                         static_cast<float>(o.noise));

  auto defaultOutputFor = [](const std::filesystem::path& input) {
    std::string base = input.stem().string();
    return (input.parent_path() / (base + ".radio.wav")).string();
  };

  std::filesystem::path startDir = std::filesystem::current_path();
  std::string initialName;
  if (!o.input.empty()) {
    std::filesystem::path inputPath(o.input);
    if (std::filesystem::exists(inputPath)) {
      if (std::filesystem::is_directory(inputPath)) {
        startDir = inputPath;
        o.input.clear();
      } else {
        startDir = inputPath.parent_path();
        initialName = toUtf8String(inputPath.filename());
      }
    }
  }

  BrowserState browser;
  browser.dir = startDir;
  refreshBrowser(browser, initialName);

  ConsoleInput input;
  input.init();

  ConsoleScreen screen;
  screen.init();

  AudioState state{};
  state.channels = channels;
  state.sampleRate = sampleRate;
  state.dry = o.dry;
  state.radio1938 = radio1938Template;
  state.useRadio1938.store(o.enableRadio);

  bool deviceReady = false;
  bool decoderReady = false;
  std::filesystem::path nowPlaying;

  auto stopM4aWorker = [&]() {
    if (!state.m4aThreadRunning.load()) return;
    state.m4aStop.store(true);
    state.m4aCv.notify_all();
    if (state.m4aThread.joinable()) {
      state.m4aThread.join();
    }
    state.m4aThreadRunning.store(false);
    state.m4aStop.store(false);
    state.m4aAtEnd.store(false);
    {
      std::lock_guard<std::mutex> lock(state.m4aMutex);
      state.m4aBuffer.reset();
      state.m4aInitDone = false;
      state.m4aInitOk = false;
      state.m4aInitError.clear();
    }
  };

  auto startM4aWorker = [&](const std::filesystem::path& file,
                            uint64_t startFrame, std::string* error) -> bool {
    stopM4aWorker();
    const uint32_t rbFrames = sampleRate * 2;
    {
      std::lock_guard<std::mutex> lock(state.m4aMutex);
      state.m4aBuffer.init(rbFrames, channels);
      state.m4aInitDone = false;
      state.m4aInitOk = false;
      state.m4aInitError.clear();
    }
    state.m4aStop.store(false);
    state.m4aAtEnd.store(false);
    state.m4aThreadRunning.store(true);
    const uint32_t workerChannels = channels;
    const uint32_t workerRate = sampleRate;
    state.m4aThread = std::thread([&, file, startFrame, workerChannels,
                                   workerRate]() {
      HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      bool comInit = SUCCEEDED(hr);
      M4aDecoder decoder;
      std::string initError;
      bool ok = decoder.init(file, workerChannels, workerRate, &initError);
      uint64_t total = 0;
      uint64_t localStart = startFrame;
      if (ok) {
        decoder.getTotalFrames(&total);
        if (total > 0 && localStart > total) {
          localStart = total;
        }
        if (localStart > 0) {
          if (!decoder.seekToFrame(localStart)) {
            localStart = 0;
          }
        }
      }
      {
        std::lock_guard<std::mutex> lock(state.m4aMutex);
        state.m4aInitDone = true;
        state.m4aInitOk = ok;
        state.m4aInitError = initError;
      }
      state.totalFrames.store(total);
      if (localStart > 0) {
        state.framesPlayed.store(localStart);
        state.audioClockFrames.store(localStart);
      }
      state.m4aCv.notify_all();
      if (!ok) {
        if (comInit) CoUninitialize();
        return;
      }

      constexpr uint32_t kChunkFrames = 2048;
      std::vector<float> buffer(
          static_cast<size_t>(kChunkFrames) * workerChannels);

      while (!state.m4aStop.load()) {
        if (state.seekRequested.load()) {
          int64_t target = state.pendingSeekFrames.load();
          if (target < 0) target = 0;
          state.seekRequested.store(false);
          uint64_t targetFrames = static_cast<uint64_t>(target);
          if (!decoder.seekToFrame(targetFrames)) {
            targetFrames = 0;
            decoder.seekToFrame(0);
          }
          state.framesPlayed.store(targetFrames);
          state.audioClockFrames.store(targetFrames);
          state.finished.store(false);
          state.m4aAtEnd.store(false);
          {
            std::lock_guard<std::mutex> lock(state.m4aMutex);
            state.m4aBuffer.reset();
          }
        }

        if (state.m4aAtEnd.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(4));
          continue;
        }

        size_t spaceFrames = 0;
        {
          std::lock_guard<std::mutex> lock(state.m4aMutex);
          spaceFrames = state.m4aBuffer.space();
        }
        if (spaceFrames == 0) {
          std::unique_lock<std::mutex> lock(state.m4aMutex);
          state.m4aCv.wait_for(lock, std::chrono::milliseconds(5), [&]() {
            return state.m4aBuffer.space() > 0 || state.m4aStop.load();
          });
          continue;
        }

        uint32_t framesToRead =
            static_cast<uint32_t>(std::min<size_t>(spaceFrames, kChunkFrames));
        uint64_t framesRead = 0;
        if (!decoder.readFrames(buffer.data(), framesToRead, &framesRead)) {
          state.m4aAtEnd.store(true);
          continue;
        }
        if (framesRead == 0) {
          state.m4aAtEnd.store(true);
          continue;
        }
        {
          std::lock_guard<std::mutex> lock(state.m4aMutex);
          state.m4aBuffer.write(buffer.data(),
                                static_cast<size_t>(framesRead),
                                workerChannels);
        }
      }
      if (comInit) CoUninitialize();
    });

    {
      std::unique_lock<std::mutex> lock(state.m4aMutex);
      state.m4aCv.wait(lock, [&]() { return state.m4aInitDone; });
      if (!state.m4aInitOk && error) {
        *error = state.m4aInitError;
      }
    }
    if (!state.m4aInitOk) {
      stopM4aWorker();
      return false;
    }
    return true;
  };

  auto initDevice = [&]() -> bool {
    if (deviceReady) return true;
    ma_device_config devConfig = ma_device_config_init(ma_device_type_playback);
    devConfig.playback.format = ma_format_f32;
    devConfig.playback.channels = channels;
    devConfig.sampleRate = sampleRate;
    devConfig.dataCallback = dataCallback;
    devConfig.pUserData = &state;

    if (ma_device_init(nullptr, &devConfig, &state.device) != MA_SUCCESS) {
      return false;
    }
    if (ma_device_start(&state.device) != MA_SUCCESS) {
      ma_device_uninit(&state.device);
      return false;
    }
    deviceReady = true;
    return true;
  };

  auto loadFileAt = [&](const std::filesystem::path& file,
                        uint64_t startFrame) -> bool {
    if (!o.enableAudio) {
      return false;
    }
    validateInputFile(file);
    const bool useM4a = isM4aExt(file);
    const bool useMiniaudio = isMiniaudioExt(file);
    if (!useM4a && !useMiniaudio) {
      return false;
    }
    if (deviceReady) {
      ma_device_stop(&state.device);
    }
    if (decoderReady) {
      if (state.usingM4a.load()) {
        stopM4aWorker();
      } else {
        ma_decoder_uninit(&state.decoder);
      }
      decoderReady = false;
      state.usingM4a.store(false);
    }

    if (useM4a) {
      std::string error;
      state.totalFrames.store(0);
      if (!startM4aWorker(file, startFrame, &error)) {
        return false;
      }
      decoderReady = true;
      state.usingM4a.store(true);
    } else {
      if (!useMiniaudio) {
        return false;
      }
      ma_decoder_config decConfig =
          ma_decoder_config_init(ma_format_f32, channels, sampleRate);
      if (ma_decoder_init_file(file.string().c_str(), &decConfig,
                               &state.decoder) != MA_SUCCESS) {
        return false;
      }
      decoderReady = true;
      state.usingM4a.store(false);

      ma_uint64 totalFrames = 0;
      if (ma_decoder_get_length_in_pcm_frames(&state.decoder, &totalFrames) ==
          MA_SUCCESS) {
        state.totalFrames.store(totalFrames);
      } else {
        state.totalFrames.store(0);
      }
    }

    if (!useM4a) {
      uint64_t total = state.totalFrames.load();
      if (total > 0 && startFrame > total) {
        startFrame = total;
      }
      if (startFrame > 0) {
        if (ma_decoder_seek_to_pcm_frame(
                &state.decoder, static_cast<ma_uint64>(startFrame)) !=
            MA_SUCCESS) {
          startFrame = 0;
        }
      }
    }

    state.framesPlayed.store(startFrame);
    state.audioClockFrames.store(startFrame);
    state.callbackCount.store(0);
    state.framesRequested.store(0);
    state.framesReadTotal.store(0);
    state.shortReadCount.store(0);
    state.silentFrames.store(0);
    state.pausedCallbacks.store(0);
    state.lastCallbackFrames.store(0);
    state.lastFramesRead.store(0);
    state.seekRequested.store(false);
    state.pendingSeekFrames.store(0);
    state.finished.store(false);
    state.paused.store(false);

    state.channels = channels;
    state.radio1938 = radio1938Template;
    state.radio1938.init(channels, static_cast<float>(sampleRate), lpHz,
                         static_cast<float>(o.noise));

    if (!deviceReady) {
      if (!initDevice()) {
        if (state.usingM4a.load()) {
          stopM4aWorker();
        } else {
          ma_decoder_uninit(&state.decoder);
        }
        decoderReady = false;
        return false;
      }
    } else {
      if (ma_device_start(&state.device) != MA_SUCCESS) {
        if (state.usingM4a.load()) {
          stopM4aWorker();
        } else {
          ma_decoder_uninit(&state.decoder);
        }
        decoderReady = false;
        return false;
      }
    }

    nowPlaying = file;
    return true;
  };

  auto loadFile = [&](const std::filesystem::path& file) -> bool {
    return loadFileAt(file, 0);
  };

  auto ensureChannels = [&](uint32_t newChannels) -> bool {
    if (newChannels == channels) return true;

    uint64_t resumeFrame = decoderReady ? state.framesPlayed.load() : 0;
    bool hadTrack = decoderReady && !nowPlaying.empty();

    if (deviceReady) {
      ma_device_stop(&state.device);
      ma_device_uninit(&state.device);
      deviceReady = false;
    }
    if (decoderReady) {
      if (state.usingM4a.load()) {
        stopM4aWorker();
      } else {
        ma_decoder_uninit(&state.decoder);
      }
      decoderReady = false;
      state.usingM4a.store(false);
    }

    channels = newChannels;
    state.channels = channels;
    radio1938Template.init(channels, static_cast<float>(sampleRate), lpHz,
                           static_cast<float>(o.noise));
    state.radio1938 = radio1938Template;

    if (hadTrack) {
      return loadFileAt(nowPlaying, resumeFrame);
    }
    return true;
  };

  auto stopPlayback = [&]() {
    if (deviceReady) {
      ma_device_stop(&state.device);
      ma_device_uninit(&state.device);
      deviceReady = false;
    }
    if (decoderReady) {
      if (state.usingM4a.load()) {
        stopM4aWorker();
      } else {
        ma_decoder_uninit(&state.decoder);
      }
      decoderReady = false;
      state.usingM4a.store(false);
    }
    state.paused.store(false);
    state.finished.store(false);
    state.seekRequested.store(false);
    state.pendingSeekFrames.store(0);
    state.framesPlayed.store(0);
    state.audioClockFrames.store(0);
    state.totalFrames.store(0);
    nowPlaying.clear();
  };

  auto renderFile = [&](const std::filesystem::path& file) -> void {
    Options renderOpt = o;
    renderOpt.input = file.string();
    if (renderOpt.output.empty()) renderOpt.output = defaultOutputFor(file);
    input.restore();
    screen.restore();
    logLine("Radioify");
    logLine(std::string("  Mode:   render"));
    logLine(std::string("  Input:  ") + renderOpt.input);
    logLine(std::string("  Output: ") + renderOpt.output);
    logLine("Rendering output...");
    renderToFile(renderOpt, radio1938Template, state.useRadio1938.load());
    logLine("Done.");
  };

  auto seekBy = [&](int direction) {
    if (!decoderReady) return;
    int64_t deltaFrames = static_cast<int64_t>(direction) * 5 * sampleRate;
    int64_t current = static_cast<int64_t>(state.audioClockFrames.load());
    int64_t target = current + deltaFrames;
    if (target < 0) target = 0;
    uint64_t total = state.totalFrames.load();
    if (total > 0 && target > static_cast<int64_t>(total)) {
      target = static_cast<int64_t>(total);
    }
    state.pendingSeekFrames.store(target);
    state.seekRequested.store(true);
    state.finished.store(false);
    state.m4aCv.notify_all();
  };

  std::filesystem::path pendingImage;
  bool hasPendingImage = false;
  std::filesystem::path pendingVideo;
  bool hasPendingVideo = false;

  auto seekToRatio = [&](double ratio) {
    uint64_t total = state.totalFrames.load();
    if (!decoderReady || total == 0) return;
    ratio = std::clamp(ratio, 0.0, 1.0);
    int64_t target = static_cast<int64_t>(ratio * static_cast<double>(total));
    state.pendingSeekFrames.store(target);
    state.seekRequested.store(true);
    state.finished.store(false);
    state.m4aCv.notify_all();
  };

  VideoPlaybackHooks videoHooks;
  if (o.enableAudio) {
    videoHooks.startAudio = [&](const std::filesystem::path& file) {
      return loadFile(file);
    };
    videoHooks.stopAudio = [&]() { stopPlayback(); };
    videoHooks.getAudioTimeSec = [&]() {
      return static_cast<double>(state.audioClockFrames.load()) / sampleRate;
    };
    videoHooks.getAudioTotalSec = [&]() -> double {
      uint64_t total = state.totalFrames.load();
      if (total == 0) return -1.0;
      return static_cast<double>(total) / sampleRate;
    };
    videoHooks.isAudioPaused = [&]() { return state.paused.load(); };
    videoHooks.isAudioFinished = [&]() { return state.finished.load(); };
    videoHooks.getAudioPerfStats = [&]() {
      AudioPerfStats stats{};
      stats.callbacks =
          state.callbackCount.load(std::memory_order_relaxed);
      stats.framesRequested =
          state.framesRequested.load(std::memory_order_relaxed);
      stats.framesRead =
          state.framesReadTotal.load(std::memory_order_relaxed);
      stats.shortReads =
          state.shortReadCount.load(std::memory_order_relaxed);
      stats.silentFrames =
          state.silentFrames.load(std::memory_order_relaxed);
      stats.lastCallbackFrames =
          state.lastCallbackFrames.load(std::memory_order_relaxed);
      stats.lastFramesRead =
          state.lastFramesRead.load(std::memory_order_relaxed);
      stats.sampleRate = state.sampleRate;
      stats.channels = state.channels;
      stats.usingM4a = state.usingM4a.load(std::memory_order_relaxed);
      if (deviceReady &&
          ma_device_get_state(&state.device) != ma_device_state_uninitialized) {
        stats.periodFrames = state.device.playback.internalPeriodSizeInFrames;
        stats.periods = state.device.playback.internalPeriods;
        stats.bufferFrames = stats.periodFrames * stats.periods;
      }
      return stats;
    };
    videoHooks.togglePause = [&]() {
      state.paused.store(!state.paused.load());
    };
    videoHooks.seekBy = [&](int direction) { seekBy(direction); };
    videoHooks.seekToRatio = [&](double ratio) { seekToRatio(ratio); };
    videoHooks.toggleRadio = [&]() {
      bool next = !state.useRadio1938.load();
      state.useRadio1938.store(next);
      uint32_t desired = next ? 1u : baseChannels;
      ensureChannels(desired);
    };
  }

  if (!o.input.empty() && o.play && std::filesystem::exists(o.input)) {
    std::filesystem::path inputPath(o.input);
    if (!std::filesystem::is_directory(inputPath)) {
      if (isSupportedImageExt(inputPath)) {
        pendingImage = inputPath;
        hasPendingImage = true;
      } else if (isVideoExt(inputPath)) {
        pendingVideo = inputPath;
        hasPendingVideo = true;
      } else {
        loadFile(inputPath);
      }
    }
  }

  const Color kBgBase{12, 15, 20};
  const Style kStyleNormal{{215, 220, 226}, kBgBase};
  const Style kStyleHeader{{230, 238, 248}, {18, 28, 44}};
  const Style kStyleHeaderGlow{{255, 213, 118}, {22, 34, 52}};
  const Style kStyleHeaderHot{{255, 249, 214}, {38, 50, 72}};
  const Style kStyleAccent{{255, 214, 120}, kBgBase};
  const Style kStyleDim{{138, 144, 153}, kBgBase};
  const Style kStyleDir{{110, 231, 183}, kBgBase};
  const Style kStyleHighlight{{15, 20, 28}, {230, 238, 248}};
  const Style kStyleBreadcrumbHover{{15, 20, 28}, {255, 214, 120}};
  const Color kProgressStart{110, 231, 183};
  const Color kProgressEnd{255, 214, 110};
  const Style kStyleProgressEmpty{{32, 38, 46}, {32, 38, 46}};
  const Style kStyleProgressFrame{{160, 170, 182}, kBgBase};

  screen.clear(kStyleNormal);
  screen.draw();

  bool running = true;
  bool dirty = true;
  auto lastDraw = std::chrono::steady_clock::now();
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
  int breadcrumbHover = -1;
  const int headerLines = 5;
  const int listTop = headerLines + 1;
  const int breadcrumbY = listTop - 1;
  const int footerLines = 3;
  int width = 0;
  int height = 0;
  int listHeight = 0;
  GridLayout layout;
  BreadcrumbLine breadcrumbLine;
  bool rendered = false;

  if (hasPendingImage) {
    showAsciiArt(pendingImage, input, screen, kStyleNormal, kStyleAccent,
                 kStyleDim);
    dirty = true;
  }
  if (hasPendingVideo) {
    bool handled = showAsciiVideo(
        pendingVideo, input, screen, kStyleNormal, kStyleAccent, kStyleDim,
        kStyleProgressEmpty, kStyleProgressFrame, kProgressStart,
        kProgressEnd, o.enableAscii, videoHooks);
    if (!handled) {
      loadFile(pendingVideo);
    }
    dirty = true;
  }

  auto rebuildLayout = [&]() {
    screen.updateSize();
    width = std::max(40, screen.width());
    height = std::max(10, screen.height());
    listHeight = height - listTop - footerLines;
    if (listHeight < 1) listHeight = 1;
    layout = buildLayout(browser, width, listHeight);
    if (layout.totalRows <= layout.rowsVisible) {
      browser.scrollRow = 0;
    } else {
      int maxScroll = layout.totalRows - layout.rowsVisible;
      browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
    }
    breadcrumbLine = buildBreadcrumbLine(browser.dir, width);
    if (breadcrumbHover >= static_cast<int>(breadcrumbLine.crumbs.size())) {
      breadcrumbHover = -1;
    }
  };

  InputCallbacks callbacks;
  callbacks.onRefreshBrowser = [&](BrowserState& nextBrowser,
                                   const std::string& initialName) {
    refreshBrowser(nextBrowser, initialName);
  };
  callbacks.onPlayFile = [&](const std::filesystem::path& file) {
    if (isSupportedImageExt(file)) {
      showAsciiArt(file, input, screen, kStyleNormal, kStyleAccent, kStyleDim);
      return true;
    }
    if (isVideoExt(file)) {
      bool handled = showAsciiVideo(
          file, input, screen, kStyleNormal, kStyleAccent, kStyleDim,
          kStyleProgressEmpty, kStyleProgressFrame, kProgressStart,
          kProgressEnd, o.enableAscii, videoHooks);
      if (handled) return true;
    }
    return loadFile(file);
  };
  callbacks.onRenderFile = [&](const std::filesystem::path& file) {
    renderFile(file);
    rendered = true;
  };
  callbacks.onTogglePause = [&]() {
    bool next = !state.paused.load();
    state.paused.store(next);
  };
  callbacks.onToggleRadio = [&]() {
    bool next = !state.useRadio1938.load();
    state.useRadio1938.store(next);
    if (o.enableAudio) {
      uint32_t desired = next ? 1u : baseChannels;
      ensureChannels(desired);
    }
  };
  callbacks.onSeekBy = [&](int direction) { seekBy(direction); };
  callbacks.onSeekToRatio = [&](double ratio) { seekToRatio(ratio); };
  callbacks.onResize = [&]() { rebuildLayout(); };

  while (running) {
    rebuildLayout();

    InputEvent ev{};
    while (input.poll(ev)) {
      handleInputEvent(ev, browser, layout, breadcrumbLine, breadcrumbY,
                       listTop, progressBarX, progressBarY, progressBarWidth,
                       o.play, decoderReady, breadcrumbHover, dirty, running,
                       callbacks);
      if (rendered) return 0;
      if (!running) break;
    }

    auto now = std::chrono::steady_clock::now();
    if (dirty || now - lastDraw >= std::chrono::milliseconds(150)) {
      screen.updateSize();
      width = std::max(40, screen.width());
      height = std::max(10, screen.height());
      listHeight = height - listTop - footerLines;
      if (listHeight < 1) listHeight = 1;
      layout = buildLayout(browser, width, listHeight);
      if (layout.totalRows <= layout.rowsVisible) {
        browser.scrollRow = 0;
      } else {
        int maxScroll = layout.totalRows - layout.rowsVisible;
        browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
      }

      screen.clear(kStyleNormal);

      const std::string titleRaw = "Radioify";
      std::string title = titleRaw;
      if (static_cast<int>(title.size()) > width) {
        title = fitLine(titleRaw, width);
      }
      int titleLen = static_cast<int>(title.size());
      int titleX = std::max(0, (width - titleLen) / 2);
      double seconds =
          std::chrono::duration<double>(now.time_since_epoch()).count();
      double speed = 1.3;
      float pulse = static_cast<float>(0.5 * (std::sin(seconds * speed) + 1.0));
      pulse = clamp01(pulse);
      pulse = pulse * pulse * (3.0f - 2.0f * pulse);
      float t = std::pow(pulse, 0.6f);
      float flash = 0.0f;
      if (t > 0.88f) {
        flash = (t - 0.88f) / 0.12f;
        flash = flash * flash;
      }
      Color headerBg = lerpColor(kStyleHeader.bg, kStyleHeaderHot.bg,
                                 std::min(0.85f, t * 0.9f));
      headerBg = lerpColor(headerBg, Color{52, 44, 26}, flash * 0.7f);
      Style headerLineStyle{kStyleHeader.fg, headerBg};
      screen.writeRun(0, 0, width, L' ', headerLineStyle);

      Color titleFg;
      if (t < 0.35f) {
        titleFg = lerpColor(kStyleHeader.fg, kStyleHeaderGlow.fg, t / 0.35f);
      } else {
        float hotT = (t - 0.35f) / 0.65f;
        titleFg =
            lerpColor(kStyleHeaderGlow.fg, kStyleHeaderHot.fg, clamp01(hotT));
      }
      if (t > 0.85f) {
        float whiteT = (t - 0.85f) / 0.15f;
        titleFg = lerpColor(titleFg, Color{255, 255, 255}, clamp01(whiteT));
      }
      if (flash > 0.0f) {
        titleFg = lerpColor(titleFg, Color{255, 236, 186}, clamp01(flash));
      }
      Style titleAttr{titleFg, headerBg};
      for (int i = 0; i < titleLen; ++i) {
        wchar_t ch = static_cast<wchar_t>(
            static_cast<unsigned char>(title[static_cast<size_t>(i)]));
        screen.writeChar(titleX + i, 0, ch, titleAttr);
      }
      breadcrumbLine = buildBreadcrumbLine(browser.dir, width);
      if (breadcrumbHover >= static_cast<int>(breadcrumbLine.crumbs.size())) {
        breadcrumbHover = -1;
      }
      screen.writeText(0, breadcrumbY, breadcrumbLine.text, kStyleAccent);
      if (breadcrumbHover >= 0) {
        const auto& crumb =
            breadcrumbLine.crumbs[static_cast<size_t>(breadcrumbHover)];
        std::string hoverText = utf8Slice(breadcrumbLine.text, crumb.startX,
                                          crumb.endX - crumb.startX);
        screen.writeText(crumb.startX, breadcrumbY, hoverText,
                         kStyleBreadcrumbHover);
      }
      if (o.play) {
        screen.writeText(
            0, 2,
            fitLine("  Mouse=select  Click=play/enter  Backspace=up  "
                    "Click+drag bar=seek  Space=pause  Arrows=move  "
                    "PgUp/PgDn=page  "
                    "Ctrl+Left/Right=seek  R=toggle  Q=quit",
                    width),
            kStyleNormal);
      } else {
        screen.writeText(
            0, 2,
            fitLine("  Mouse=select  Click=render/enter  Backspace=up  "
                    "Arrows=move  PgUp/PgDn=page  R=toggle  Q=quit",
                    width),
            kStyleNormal);
      }
      std::string filterLabel =
          state.useRadio1938.load() ? "1938 radio" : "dry";
      screen.writeText(0, 3,
                       fitLine(std::string("  Filter: ") + filterLabel, width),
                       kStyleDim);
      screen.writeText(
          0, 4,
          fitLine(
              "  Showing: folders + .wav/.mp3/.flac/.m4a/.webm/.mp4/.jpg/.jpeg/.png/.bmp",
              width),
          kStyleDim);

      if (browser.entries.empty()) {
        screen.writeText(2, listTop, "(no supported files)", kStyleDim);
      } else {
        for (int r = 0; r < layout.rowsVisible; ++r) {
          int y = listTop + r;
          int logicalRow = r + browser.scrollRow;
          if (logicalRow >= layout.totalRows) continue;
          for (int c = 0; c < layout.cols; ++c) {
            int idx = c * layout.totalRows + logicalRow;
            if (idx >= static_cast<int>(browser.entries.size())) continue;
            const auto& entry = browser.entries[static_cast<size_t>(idx)];
            bool isSelected = (idx == browser.selected);
            std::string cell = fitName(layout.names[static_cast<size_t>(idx)],
                                       layout.colWidth);
            int cellWidth = utf8CodepointCount(cell);
            if (cellWidth < layout.colWidth) {
              cell.append(static_cast<size_t>(layout.colWidth - cellWidth),
                          ' ');
            } else if (cellWidth > layout.colWidth) {
              cell = utf8Take(cell, layout.colWidth);
            }
            Style attr = isSelected ? kStyleHighlight
                                    : (entry.isDir ? kStyleDir : kStyleNormal);
            screen.writeText(c * layout.colWidth, y, cell, attr);
          }
        }
      }

      int footerStart = listTop + listHeight;
      int line = footerStart;
      if (line < height) {
        line++;
      }
      std::string nowLabel =
          nowPlaying.empty() ? "(none)" : toUtf8String(nowPlaying.filename());
      screen.writeText(0, line++, fitLine(std::string(" ") + nowLabel, width),
                       kStyleAccent);

      double currentSec =
          decoderReady
              ? static_cast<double>(state.audioClockFrames.load()) / sampleRate
              : 0.0;
      double totalSec =
          (decoderReady && state.totalFrames.load() > 0)
              ? static_cast<double>(state.totalFrames.load()) / sampleRate
              : -1.0;
      std::string status;
      if (decoderReady) {
        if (state.finished.load()) {
          status = "\xE2\x96\xA0";  // ended icon
        } else if (state.paused.load()) {
          status = "\xE2\x8F\xB8";  // paused icon
        } else {
          status = "\xE2\x96\xB6";  // playing icon
        }
      } else {
        status = "\xE2\x97\x8B";  // idle icon
      }
      std::string suffix =
          formatTime(currentSec) + " / " + formatTime(totalSec) + " " + status;
      int suffixWidth = utf8CodepointCount(suffix);
      int barWidth = width - suffixWidth - 3;
      if (barWidth < 10) {
        suffix = formatTime(currentSec) + "/" + formatTime(totalSec);
        suffixWidth = utf8CodepointCount(suffix);
        barWidth = width - suffixWidth - 3;
      }
      if (barWidth < 10) {
        suffix = formatTime(currentSec);
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
        ratio = std::clamp(currentSec / totalSec, 0.0, 1.0);
      }
      progressBarX = 1;
      progressBarY = line;
      progressBarWidth = barWidth;

      screen.writeChar(0, line, L'|', kStyleProgressFrame);
      auto barCells = renderProgressBarCells(ratio, barWidth,
                                             kStyleProgressEmpty,
                                             kProgressStart, kProgressEnd);
      for (int i = 0; i < barWidth; ++i) {
        const auto& cell = barCells[static_cast<size_t>(i)];
        screen.writeChar(1 + i, line, cell.ch, cell.style);
      }
      screen.writeChar(1 + barWidth, line, L'|', kStyleProgressFrame);
      if (!suffix.empty()) {
        screen.writeText(2 + barWidth, line, " " + suffix, kStyleNormal);
      }

      screen.draw();
      lastDraw = now;
      dirty = false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Clear screen before exiting
  screen.clear(kStyleNormal);
  screen.draw();
  input.restore();
  screen.restore();
  std::cout << "\n";

  if (deviceReady) {
    ma_device_uninit(&state.device);
  }
  if (decoderReady) {
    if (state.usingM4a.load()) {
      stopM4aWorker();
    } else {
      ma_decoder_uninit(&state.decoder);
    }
  }
  return 0;
}
