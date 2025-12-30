#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <new>
#include <thread>
#include <utility>
#include <vector>

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

static std::string parseInputPath(int argc, char** argv) {
  if (argc <= 1) return {};
  if (argc > 2) die("Provide a single file or folder path only.");
  return std::string(argv[1]);
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

struct VideoPlaybackHooks {
  std::function<bool(const std::filesystem::path&)> startAudio;
  std::function<void()> stopAudio;
  std::function<double()> getAudioTimeSec;
  std::function<double()> getAudioTotalSec;
  std::function<bool()> isAudioPaused;
  std::function<bool()> isAudioFinished;
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
    const VideoPlaybackHooks& hooks) {
  VideoDecoder decoder;
  std::string error;
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

  if (!decoder.init(file, &error)) {
    if (error.rfind("No video stream found", 0) == 0) {
      bool playAudio = showAudioFallbackPrompt(
          "No video stream found.",
          "This file can be played as audio only.");
      return playAudio ? false : true;
    }
    return showError(error, "");
  }

  PerfLog perfLog;
  std::string logError;
  std::filesystem::path logPath =
      std::filesystem::current_path() / "radioify_timing.log";
  if (!perfLog.open(logPath, &logError)) {
    return showError("Failed to open timing log file.", logError);
  }
  perfLog.appendf("video_start file=%s", toUtf8String(file.filename()).c_str());

  bool audioOk = hooks.startAudio ? hooks.startAudio(file) : false;
  perfLog.appendf("audio_start ok=%d", audioOk ? 1 : 0);
  bool running = true;
  bool videoEnded = false;
  bool redraw = true;
  AsciiArt art;
  VideoFrame frame;
  VideoFrame nextFrame;
  int cachedWidth = -1;
  int cachedMaxHeight = -1;
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
  bool renderFailed = false;
  std::string renderFailMessage;
  std::string renderFailDetail;

  if (!decoder.readFrame(frame)) {
    if (hooks.stopAudio) hooks.stopAudio();
    return showError("No video frames found.", "");
  }
  perfLog.appendf("first_frame ts100ns=%lld size=%dx%d",
                  static_cast<long long>(frame.timestamp100ns), frame.width,
                  frame.height);

  const double ticksToSeconds = 1.0 / 10000000.0;
  const double secondsToTicks = 10000000.0;
  int64_t firstTs = frame.timestamp100ns;
  double frameSec =
      static_cast<double>(frame.timestamp100ns - firstTs) * ticksToSeconds;
  double lastFrameSec = frameSec;
  auto startTime = std::chrono::steady_clock::now();
  auto lastUiUpdate = startTime;

  auto totalDurationSec = [&]() -> double {
    double total = decoder.duration100ns() > 0
                       ? decoder.duration100ns() * ticksToSeconds
                       : -1.0;
    if (total <= 0.0 && audioOk && hooks.getAudioTotalSec) {
      total = hooks.getAudioTotalSec();
    }
    return total;
  };

  auto currentTimeSec = [&]() -> double {
    if (audioOk && hooks.getAudioTimeSec) {
      return hooks.getAudioTimeSec();
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
        (width != cachedWidth || maxHeight != cachedMaxHeight);
    if (refreshArt || sizeChanged) {
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
    }

    screen.clear(baseStyle);
    std::string title = "Video: " + toUtf8String(file.filename());
    screen.writeText(0, 0, fitLine(title, width), accentStyle);
    std::string subtitle = audioOk ? "" : "Audio unavailable";
    if (!subtitle.empty()) {
      screen.writeText(0, 1, fitLine(subtitle, width), dimStyle);
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
    if (hooks.stopAudio) hooks.stopAudio();
    return showError(renderFailMessage, renderFailDetail);
  }

  while (running) {
    InputEvent ev{};
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Resize) {
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
          if (decoder.seekToTimestamp100ns(targetTs) &&
              decoder.readFrame(frame)) {
            frameSec = static_cast<double>(frame.timestamp100ns - firstTs) *
                       ticksToSeconds;
            lastFrameSec = frameSec;
            videoEnded = false;
            redraw = true;
          }
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
                if (decoder.seekToTimestamp100ns(targetTs) &&
                    decoder.readFrame(frame)) {
                  frameSec =
                      static_cast<double>(frame.timestamp100ns - firstTs) *
                      ticksToSeconds;
                  lastFrameSec = frameSec;
                  videoEnded = false;
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
    double syncSec = audioOk && hooks.getAudioTimeSec
                         ? hooks.getAudioTimeSec()
                         : std::chrono::duration<double>(now - startTime)
                               .count();
    const double leadSlack = 0.005;
    const double catchupThreshold = 0.12;

    if (!videoEnded && !paused) {
      if (syncSec + leadSlack >= frameSec) {
        double displayedSec = frameSec;
        auto renderStart = std::chrono::steady_clock::now();
        renderScreen(true);
        auto renderEnd = std::chrono::steady_clock::now();
        if (renderFailed) {
          running = false;
          break;
        }
        redraw = false;
        lastFrameSec = frameSec;
        auto decodeStart = std::chrono::steady_clock::now();
        bool decoded = decoder.readFrame(nextFrame);
        int dropped = 0;
        if (decoded) {
          frame = std::move(nextFrame);
          frameSec =
              static_cast<double>(frame.timestamp100ns - firstTs) *
              ticksToSeconds;
          while (syncSec - frameSec > catchupThreshold) {
            if (!decoder.readFrame(nextFrame)) {
              videoEnded = true;
              break;
            }
            frame = std::move(nextFrame);
            frameSec =
                static_cast<double>(frame.timestamp100ns - firstTs) *
                ticksToSeconds;
            ++dropped;
          }
        }
        auto decodeEnd = std::chrono::steady_clock::now();
        double renderMs =
            std::chrono::duration<double, std::milli>(renderEnd - renderStart)
                .count();
        double decodeMs =
            std::chrono::duration<double, std::milli>(decodeEnd - decodeStart)
                .count();
        double lagSec = syncSec - displayedSec;
        if (decoded) {
          perfLog.appendf(
              "frame t=%.3f sync=%.3f lag=%.3f render_ms=%.2f decode_ms=%.2f dropped=%d",
              displayedSec, syncSec, lagSec, renderMs, decodeMs, dropped);
        } else {
          perfLog.appendf(
              "frame t=%.3f sync=%.3f lag=%.3f render_ms=%.2f decode_ms=%.2f dropped=0 eof=1",
              displayedSec, syncSec, lagSec, renderMs, decodeMs);
          videoEnded = true;
        }
      }
    }

    if (redraw) {
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

struct AudioState {
  ma_decoder decoder{};
  M4aDecoder m4a{};
  ma_device device{};
  Radio1938 radio1938{};
  std::atomic<bool> paused{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> useRadio1938{true};
  std::atomic<bool> usingM4a{false};
  std::atomic<bool> seekRequested{false};
  std::atomic<int64_t> pendingSeekFrames{0};
  std::atomic<uint64_t> framesPlayed{0};
  uint64_t totalFrames = 0;
  uint32_t channels = 1;
  uint32_t sampleRate = 48000;
  bool dry = false;
};

static void dataCallback(ma_device* device, void* output, const void*,
                         ma_uint32 frameCount) {
  auto* state = static_cast<AudioState*>(device->pUserData);
  float* out = static_cast<float*>(output);
  if (!state) return;

  if (state->seekRequested.exchange(false)) {
    int64_t target = state->pendingSeekFrames.load();
    if (target < 0) target = 0;
    if (state->usingM4a.load()) {
      if (!state->m4a.seekToFrame(static_cast<uint64_t>(target))) {
        target = 0;
        state->m4a.seekToFrame(0);
      }
    } else {
      ma_decoder_seek_to_pcm_frame(&state->decoder,
                                   static_cast<ma_uint64>(target));
    }
    state->framesPlayed.store(static_cast<uint64_t>(target));
    state->finished.store(false);
  }

  if (state->paused.load()) {
    std::fill(out, out + frameCount * state->channels, 0.0f);
    return;
  }

  uint64_t framesRead = 0;
  if (state->usingM4a.load()) {
    if (!state->m4a.readFrames(out, frameCount, &framesRead)) {
      state->finished.store(true);
      return;
    }
    if (framesRead < frameCount) {
      std::fill(out + framesRead * state->channels,
                out + frameCount * state->channels, 0.0f);
    }
    if (framesRead == 0) {
      state->finished.store(true);
      if (state->totalFrames > 0) {
        state->framesPlayed.store(state->totalFrames);
      }
    }
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
      if (state->totalFrames > 0) {
        state->framesPlayed.store(state->totalFrames);
      }
    }
  }

  if (!state->dry && framesRead > 0 && state->useRadio1938.load()) {
    state->radio1938.process(out, static_cast<uint32_t>(framesRead));
  }
  state->framesPlayed.fetch_add(framesRead);
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
  Options o;
  o.input = parseInputPath(argc, argv);

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
  state.useRadio1938.store(true);

  bool deviceReady = false;
  bool decoderReady = false;
  std::filesystem::path nowPlaying;

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
    validateInputFile(file);
    const bool useM4a = isM4aExt(file);
    if (deviceReady) {
      ma_device_stop(&state.device);
    }
    if (decoderReady) {
      if (state.usingM4a.load()) {
        state.m4a.uninit();
      } else {
        ma_decoder_uninit(&state.decoder);
      }
      decoderReady = false;
      state.usingM4a.store(false);
    }

    if (useM4a) {
      std::string error;
      if (!state.m4a.init(file, channels, sampleRate, &error)) {
        return false;
      }
      decoderReady = true;
      state.usingM4a.store(true);
      uint64_t totalFrames = 0;
      if (state.m4a.getTotalFrames(&totalFrames)) {
        state.totalFrames = totalFrames;
      } else {
        state.totalFrames = 0;
      }
    } else {
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
        state.totalFrames = totalFrames;
      } else {
        state.totalFrames = 0;
      }
    }

    if (state.totalFrames > 0 && startFrame > state.totalFrames) {
      startFrame = state.totalFrames;
    }
    if (startFrame > 0) {
      if (state.usingM4a.load()) {
        if (!state.m4a.seekToFrame(startFrame)) {
          startFrame = 0;
        }
      } else {
        if (ma_decoder_seek_to_pcm_frame(&state.decoder,
                                         static_cast<ma_uint64>(startFrame)) !=
            MA_SUCCESS) {
          startFrame = 0;
        }
      }
    }

    state.framesPlayed.store(startFrame);
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
          state.m4a.uninit();
        } else {
          ma_decoder_uninit(&state.decoder);
        }
        decoderReady = false;
        return false;
      }
    } else {
      if (ma_device_start(&state.device) != MA_SUCCESS) {
        if (state.usingM4a.load()) {
          state.m4a.uninit();
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
        state.m4a.uninit();
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
        state.m4a.uninit();
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
    state.totalFrames = 0;
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
    int64_t current = static_cast<int64_t>(state.framesPlayed.load());
    int64_t target = current + deltaFrames;
    if (target < 0) target = 0;
    if (state.totalFrames > 0 &&
        target > static_cast<int64_t>(state.totalFrames)) {
      target = static_cast<int64_t>(state.totalFrames);
    }
    state.pendingSeekFrames.store(target);
    state.seekRequested.store(true);
    state.finished.store(false);
  };

  std::filesystem::path pendingImage;
  bool hasPendingImage = false;
  std::filesystem::path pendingVideo;
  bool hasPendingVideo = false;

  auto seekToRatio = [&](double ratio) {
    if (!decoderReady || state.totalFrames == 0) return;
    ratio = std::clamp(ratio, 0.0, 1.0);
    int64_t target =
        static_cast<int64_t>(ratio * static_cast<double>(state.totalFrames));
    state.pendingSeekFrames.store(target);
    state.seekRequested.store(true);
    state.finished.store(false);
  };

  VideoPlaybackHooks videoHooks;
  videoHooks.startAudio = [&](const std::filesystem::path& file) {
    return loadFile(file);
  };
  videoHooks.stopAudio = [&]() { stopPlayback(); };
  videoHooks.getAudioTimeSec = [&]() {
    return static_cast<double>(state.framesPlayed.load()) / sampleRate;
  };
  videoHooks.getAudioTotalSec = [&]() -> double {
    if (state.totalFrames == 0) return -1.0;
    return static_cast<double>(state.totalFrames) / sampleRate;
  };
  videoHooks.isAudioPaused = [&]() { return state.paused.load(); };
  videoHooks.isAudioFinished = [&]() { return state.finished.load(); };
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
        kProgressEnd, videoHooks);
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
          kProgressEnd, videoHooks);
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
    uint32_t desired = next ? 1u : baseChannels;
    ensureChannels(desired);
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
              ? static_cast<double>(state.framesPlayed.load()) / sampleRate
              : 0.0;
      double totalSec =
          (decoderReady && state.totalFrames > 0)
              ? static_cast<double>(state.totalFrames) / sampleRate
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
      state.m4a.uninit();
    } else {
      ma_decoder_uninit(&state.decoder);
    }
  }
  return 0;
}
