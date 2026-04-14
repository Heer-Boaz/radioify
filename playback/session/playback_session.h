#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "playback/playback_transport.h"
#include "videoplayback.h"

class PlaybackSession {
 public:
  struct Args {
    const std::filesystem::path& file;
    ConsoleInput& input;
    ConsoleScreen& screen;
    const Style& baseStyle;
    const Style& accentStyle;
    const Style& dimStyle;
    const Style& progressEmptyStyle;
    const Style& progressFrameStyle;
    const Color& progressStart;
    const Color& progressEnd;
    const VideoPlaybackConfig& config;
    bool* quitAppRequested = nullptr;
    std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
  };

  explicit PlaybackSession(Args args);
  ~PlaybackSession();

  PlaybackSession(PlaybackSession&&) noexcept;
  PlaybackSession& operator=(PlaybackSession&&) noexcept;

  PlaybackSession(const PlaybackSession&) = delete;
  PlaybackSession& operator=(const PlaybackSession&) = delete;

  bool run();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
