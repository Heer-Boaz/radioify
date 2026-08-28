#pragma once

#include <filesystem>
#include <memory>

#include "playback/session/open_outcome.h"

class ConsoleInput;
class ConsoleScreen;
class Player;
struct Color;
struct Style;

class PlaybackSessionBootstrap {
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
    bool enableAudio;
    bool enableAscii;
    Player& player;
  };

  explicit PlaybackSessionBootstrap(Args args);
  ~PlaybackSessionBootstrap();

  PlaybackSessionBootstrap(const PlaybackSessionBootstrap&) = delete;
  PlaybackSessionBootstrap& operator=(const PlaybackSessionBootstrap&) = delete;

  PlaybackSessionBootstrap(PlaybackSessionBootstrap&&) noexcept;
  PlaybackSessionBootstrap& operator=(PlaybackSessionBootstrap&&) noexcept;

  playback_session::OpenOutcome run();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
