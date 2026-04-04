#include "playback_session_host.h"

#include <fstream>
#include <mutex>
#include <string>

#include "consoleinput.h"
#include "consolescreen.h"
#include "gpu_shared.h"
#include "playback_dialog.h"
#include "runtime_helpers.h"
#include "subtitle_manager.h"
#include "timing_log.h"
#include "ui_helpers.h"
#include "videoplayback.h"

PlaybackSessionHost::PlaybackSessionHost(const Args& args)
    : input_(args.input),
      screen_(args.screen),
      baseStyle_(args.baseStyle),
      accentStyle_(args.accentStyle),
      dimStyle_(args.dimStyle),
      fullRedrawEnabled_(args.enableAscii),
      quitAppRequested_(args.quitAppRequested),
      logPath_(radioifyLogPath()),
      windowTitle_(toUtf8String(args.file.filename())) {
  if (quitAppRequested_) {
    *quitAppRequested_ = false;
  }
  sharedGpuRenderer().ResetSessionState();
  if (fullRedrawEnabled_) {
    screen_.setAlwaysFullRedraw(true);
  }
}

PlaybackSessionHost::~PlaybackSessionHost() {
  if (quitAppRequested_) {
    *quitAppRequested_ = quitApplicationRequested_;
  }
  finalizeVideoPlayback(screen_, fullRedrawEnabled_, &perfLog_);
  sharedGpuRenderer().ResetSessionState();
}

bool PlaybackSessionHost::initialize() {
  std::string logError;
  configureFfmpegVideoLog(logPath_);
  if (!perfLogOpen(&perfLog_, logPath_, &logError)) {
    playback_dialog::showInfoDialog(input_, screen_, baseStyle_, accentStyle_,
                                    dimStyle_, "Video error",
                                    "Failed to open timing log file.", logError,
                                    "");
    return false;
  }
  perfLogAppendf(&perfLog_, "video_start file=%s", windowTitle_.c_str());
  return true;
}

void PlaybackSessionHost::logSubtitleDetection(
    const SubtitleManager& subtitleManager) {
  perfLogAppendf(&perfLog_, "subtitle_detect tracks=%zu usable=%zu active=%s",
                 subtitleManager.trackCount(),
                 subtitleManager.selectableTrackCount(),
                 subtitleManager.activeTrackLabel().c_str());
}

bool PlaybackSessionHost::reportVideoError(const std::string& message,
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
  std::ofstream f(logPath_, std::ios::app);
  if (f) {
    f << radioifyLogTimestamp() << " " << payload << "\n";
  }
#else
  (void)message;
  (void)detail;
#endif

  std::string uiMessage =
      message.empty() ? "Video playback error." : message;
  std::string uiDetail = detail;
  if (uiMessage.empty() && uiDetail.empty()) {
    uiDetail = "Video playback encountered an unexpected error.";
  }
  playback_dialog::showInfoDialog(input_, screen_, baseStyle_, accentStyle_,
                                  dimStyle_, "Video error", uiMessage,
                                  uiDetail, "");
  return true;
}

PerfLog& PlaybackSessionHost::perfLog() { return perfLog_; }

playback_frame_output::LogLineWriter PlaybackSessionHost::timingSink() const {
#if RADIOIFY_ENABLE_TIMING_LOG
  PerfLog* perfLog = const_cast<PerfLog*>(&perfLog_);
  return [perfLog](const std::string& line) {
    if (line.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(timingLogMutex());
    if (perfLog->file.is_open()) {
      perfLog->file << radioifyLogTimestamp() << " " << line << "\n";
    }
  };
#else
  return [](const std::string& line) { (void)line; };
#endif
}

playback_frame_output::LogLineWriter PlaybackSessionHost::warningSink() const {
#if RADIOIFY_ENABLE_VIDEO_ERROR_LOG
  const std::filesystem::path logPath = logPath_;
  return [logPath](const std::string& message) {
    const std::string line = message.empty() ? "Video warning." : message;
    const std::string payload = "video_warning msg=" + line;
    std::ofstream f(logPath, std::ios::app);
    if (f) {
      f << radioifyLogTimestamp() << " " << payload << "\n";
    }
  };
#else
  return [](const std::string& message) { (void)message; };
#endif
}

const std::string& PlaybackSessionHost::windowTitle() const {
  return windowTitle_;
}

bool* PlaybackSessionHost::quitApplicationRequestedPtr() {
  return &quitApplicationRequested_;
}
