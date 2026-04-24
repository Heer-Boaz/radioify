#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "playback/system_media_transport/controls.h"
#include "playback/control/transport.h"
#include "playback/ascii/frame_output.h"
#include "playback/session/state.h"
#include "log.h"
#include "playback/video/playback.h"

class Player;
class SubtitleManager;

class PlaybackLoopRunner {
 public:
  struct Args {
    ConsoleInput& input;
    ConsoleScreen& screen;
    const VideoPlaybackConfig& config;
    Player& player;
    SubtitleManager& subtitleManager;
    PerfLog& perfLog;
    const Style& baseStyle;
    const Style& accentStyle;
    const Style& dimStyle;
    const Style& progressEmptyStyle;
    const Style& progressFrameStyle;
    const Color& progressStart;
    const Color& progressEnd;
    playback_frame_output::LogLineWriter timingSink;
    playback_frame_output::LogLineWriter warningSink;
    std::atomic<bool>& enableSubtitlesShared;
    const std::string& windowTitle;
    const std::filesystem::path& file;
    bool enableAscii;
    bool enableAudio;
    bool hasSubtitles = false;
    bool* quitApplicationRequested = nullptr;
    PlaybackSystemControls* systemControls = nullptr;
    std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
    std::function<bool(const std::vector<std::filesystem::path>&)> requestOpenFiles;
    PlaybackSessionContinuationState* continuityState = nullptr;
  };

  explicit PlaybackLoopRunner(Args args);
  ~PlaybackLoopRunner();

  PlaybackLoopRunner(PlaybackLoopRunner&&) noexcept;
  PlaybackLoopRunner& operator=(PlaybackLoopRunner&&) noexcept;

  PlaybackLoopRunner(const PlaybackLoopRunner&) = delete;
  PlaybackLoopRunner& operator=(const PlaybackLoopRunner&) = delete;

  void run();
  void shutdown();
  void renderFailureScreen();
  PlaybackSessionContinuationState continuationState() const;

  bool hasRenderFailure() const;
  const std::string& renderFailureMessage() const;
  const std::string& renderFailureDetail() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
