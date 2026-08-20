#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "playback/control/transport.h"
#include "playback/session/state.h"

class ConsoleInput;
class ConsoleScreen;
class OpenFileRequests;
class PlaybackNotificationAreaControls;
class PlaybackSystemControls;
struct Color;
struct Style;
struct VideoPlaybackConfig;

enum class PlaybackSessionOpenOutcome {
  Ready,
  HandledWithoutPlayback,
  AudioFallbackRequested,
};

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
    OpenFileRequests& openFileRequests;
    bool* quitAppRequested = nullptr;
    PlaybackSystemControls* systemControls = nullptr;
    PlaybackNotificationAreaControls* notificationAreaControls = nullptr;
    std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
    std::function<bool(const std::vector<std::filesystem::path>&)> requestOpenFiles;
    PlaybackSessionContinuationState* continuityState = nullptr;
    PlaybackSessionIntent sessionIntent = PlaybackSessionIntent::View;
  };

  explicit PlaybackSession(Args args);
  ~PlaybackSession();

  PlaybackSession(PlaybackSession&&) noexcept;
  PlaybackSession& operator=(PlaybackSession&&) noexcept;

  PlaybackSession(const PlaybackSession&) = delete;
  PlaybackSession& operator=(const PlaybackSession&) = delete;

  PlaybackSessionOpenOutcome open();
  void run();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
