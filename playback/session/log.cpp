#include "log.h"

#include <chrono>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

extern "C" {
#include <libavutil/log.h>
}

#include "consolescreen.h"
#include "timing_log.h"
#include "ui_helpers.h"

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

  // Simple deduplication to prevent flooding during seeking storms.
  static std::string lastLine;
  static int repeatCount = 0;
  static auto lastLogTime = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();

  std::string currentLine(line);
  if (currentLine == lastLine && (now - lastLogTime < std::chrono::seconds(1))) {
    repeatCount++;
    if (repeatCount == 100) {
      gFfmpegLogFile << radioifyLogTimestamp()
                     << " ffmpeg_error ... multiple repeats suppressed ...\n";
    }
    return;
  }

  if (repeatCount > 0 && repeatCount != 100) {
    gFfmpegLogFile << radioifyLogTimestamp()
                   << " ffmpeg_error (repeated " << repeatCount << " times)\n";
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
}  // namespace

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
