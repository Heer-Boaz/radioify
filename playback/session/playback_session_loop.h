#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "playback/playback_transport.h"
#include "playback/ascii/playback_frame_output.h"
#include "playback_session_log.h"
#include "videoplayback.h"

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
    bool enableAscii = true;
    bool enableAudio = true;
    bool hasSubtitles = false;
    bool* quitApplicationRequested = nullptr;
    std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
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

  bool hasRenderFailure() const;
  const std::string& renderFailureMessage() const;
  const std::string& renderFailureDetail() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
