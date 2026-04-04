#pragma once

#include <filesystem>
#include <memory>

class ConsoleInput;
class ConsoleScreen;
class Player;
struct Color;
struct Style;

enum class PlaybackSessionBootstrapOutcome {
  ContinueVideo,
  PlayAudioOnly,
  Handled,
};

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
    bool enableAudio = true;
    bool enableAscii = true;
    Player& player;
    bool* quitAppRequested = nullptr;
  };

  explicit PlaybackSessionBootstrap(Args args);
  ~PlaybackSessionBootstrap();

  PlaybackSessionBootstrap(const PlaybackSessionBootstrap&) = delete;
  PlaybackSessionBootstrap& operator=(const PlaybackSessionBootstrap&) = delete;

  PlaybackSessionBootstrap(PlaybackSessionBootstrap&&) noexcept;
  PlaybackSessionBootstrap& operator=(PlaybackSessionBootstrap&&) noexcept;

  PlaybackSessionBootstrapOutcome run();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
