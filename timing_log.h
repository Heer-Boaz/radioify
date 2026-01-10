#ifndef RADIOIFY_TIMING_LOG_H
#define RADIOIFY_TIMING_LOG_H

#include <chrono>
#include <ctime>
#include <string>
#include <cstdio>
#include <mutex>

inline std::mutex& timingLogMutex() {
  static std::mutex m;
  return m;
}

inline std::string radioifyLogTimestamp() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  std::time_t tt = system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
  return std::string(buf);
}

#ifndef RADIOIFY_ENABLE_TIMING_LOG
#define RADIOIFY_ENABLE_TIMING_LOG 0
#endif

#ifndef RADIOIFY_ENABLE_VIDEO_ERROR_LOG
#define RADIOIFY_ENABLE_VIDEO_ERROR_LOG 0
#endif

#ifndef RADIOIFY_ENABLE_FFMPEG_ERROR_LOG
#define RADIOIFY_ENABLE_FFMPEG_ERROR_LOG 0
#endif

#endif
