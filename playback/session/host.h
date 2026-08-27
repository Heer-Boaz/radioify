#pragma once

#include <filesystem>
#include <string>

#include "playback/ascii/frame_output.h"
#include "log.h"

class ConsoleInput;
class ConsoleScreen;
class GpuRuntime;
class SubtitleManager;
struct Style;

class PlaybackSessionHost {
 public:
  struct Args {
    const std::filesystem::path& file;
    ConsoleInput& input;
    ConsoleScreen& screen;
    GpuRuntime& gpu;
    const Style& baseStyle;
    const Style& accentStyle;
    const Style& dimStyle;
    bool enableAscii;
  };

  explicit PlaybackSessionHost(const Args& args);
  ~PlaybackSessionHost();

  PlaybackSessionHost(const PlaybackSessionHost&) = delete;
  PlaybackSessionHost& operator=(const PlaybackSessionHost&) = delete;

  bool initialize();
  void logSubtitleDetection(const SubtitleManager& subtitleManager);
  bool reportVideoError(const std::string& message, const std::string& detail);

  PerfLog& perfLog();
  playback_frame_output::LogLineWriter timingSink() const;
  playback_frame_output::LogLineWriter warningSink() const;
  const std::string& windowTitle() const;

 private:
  ConsoleInput& input_;
  ConsoleScreen& screen_;
  GpuRuntime& gpu_;
  const Style& baseStyle_;
  const Style& accentStyle_;
  const Style& dimStyle_;
  const bool fullRedrawEnabled_;
  PerfLog perfLog_;
  std::filesystem::path logPath_;
  std::string windowTitle_;
};
