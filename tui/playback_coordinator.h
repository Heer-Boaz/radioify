#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "app/playback_queue.h"

class ConsoleInput;
class ConsoleScreen;
class OpenFileRequests;
struct OpenFilesRequest;
class PlaybackNotificationAreaControls;
class PlaybackSystemControls;
struct Color;
struct Style;
struct VideoPlaybackConfig;

class TuiPlaybackCoordinator {
 public:
  struct Services {
    playback_queue::Queue& queue;
    ConsoleInput& input;
    ConsoleScreen& screen;
    const Style& baseStyle;
    const Style& accentStyle;
    const Style& dimStyle;
    const Style& progressEmptyStyle;
    const Style& progressFrameStyle;
    const Color& progressStart;
    const Color& progressEnd;
    const VideoPlaybackConfig& videoConfig;
    OpenFileRequests& openFileRequests;
    PlaybackSystemControls& systemControls;
    PlaybackNotificationAreaControls& notificationAreaControls;
    std::function<bool(const std::filesystem::path&, int)> startAudio;
    std::function<void(playback_route::AudioPictureInPicturePlan)>
        applyAudioPictureInPicturePlan;
    std::function<bool(const OpenFilesRequest&)> requestOpenFiles;
    std::function<void()> requestQuit;
    std::function<void()> presentationFinished;
  };

  explicit TuiPlaybackCoordinator(Services services);
  ~TuiPlaybackCoordinator();

  TuiPlaybackCoordinator(const TuiPlaybackCoordinator&) = delete;
  TuiPlaybackCoordinator& operator=(const TuiPlaybackCoordinator&) = delete;

  [[nodiscard]] bool start(playback_route::Route route,
                           playback_queue::Source source);
  [[nodiscard]] bool transport(playback_queue::Direction direction);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
