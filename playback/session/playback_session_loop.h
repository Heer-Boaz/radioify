#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "playback/system_media_transport_controls.h"
#include "playback/playback_transport.h"
#include "playback/ascii/playback_frame_output.h"
#include "playback/session/playback_session_state.h"
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
    const std::filesystem::path& file;
    bool enableAscii;
    bool enableAudio;
    bool hasSubtitles = false;
    bool* quitApplicationRequested = nullptr;
    PlaybackSystemControls* systemControls = nullptr;
    std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
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
