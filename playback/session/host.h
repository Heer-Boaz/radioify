#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "playback/ascii/frame_output.h"
#include "playback/session/open_outcome.h"
#include "log.h"

class ConsoleScreen;
class GpuRuntime;
class SubtitleManager;

class PlaybackSessionHost {
 public:
  struct Args {
    const std::filesystem::path& file;
    ConsoleScreen& screen;
    GpuRuntime& gpu;
    bool enableAscii;
  };

  explicit PlaybackSessionHost(const Args& args);
  ~PlaybackSessionHost();

  PlaybackSessionHost(const PlaybackSessionHost&) = delete;
  PlaybackSessionHost& operator=(const PlaybackSessionHost&) = delete;

  std::optional<playback_session::Problem> tryInitialize();
  void logSubtitleDetection(const SubtitleManager& subtitleManager);
  playback_session::Problem recordVideoError(const std::string& message,
                                             const std::string& detail);

  PerfLog& perfLog();
  playback_frame_output::LogLineWriter timingSink() const;
  playback_frame_output::LogLineWriter warningSink() const;
  const std::string& windowTitle() const;

 private:
  ConsoleScreen& screen_;
  GpuRuntime& gpu_;
  const bool fullRedrawEnabled_;
  PerfLog perfLog_;
  std::filesystem::path logPath_;
  std::string windowTitle_;
};
