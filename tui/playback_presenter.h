#pragma once

#include <filesystem>
#include <functional>
#include <vector>

#include "app/playback_controller.h"
#include "playback/session/state.h"

class ConsoleInput;
class ConsoleScreen;
class OpenFileRequests;
struct OpenFilesRequest;
class PlaybackNotificationAreaControls;
class PlaybackSystemControls;
struct Color;
struct Style;
struct VideoPlaybackConfig;

class TuiPlaybackPresenter final : public playback_controller::Presenter {
 public:
  struct Services {
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

  explicit TuiPlaybackPresenter(Services services);

  [[nodiscard]] playback_controller::PresentationOpenResult open(
      const playback_route::Route& route,
      playback_controller::SessionCommands& commands) override;

 private:
  Services services_;
  PlaybackSessionContinuationState continuationState_;
};
