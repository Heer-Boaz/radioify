#include "playback_presenter.h"

#include <memory>
#include <utility>

#include "core/open_file_requests.h"
#include "media_formats.h"
#include "playback/control/transport.h"
#include "playback/session/session.h"
#include "playback/video/playback.h"

namespace {

VideoPlaybackConfig sessionConfig(
    const VideoPlaybackConfig& base,
    const PlaybackSessionContinuationState& continuation) {
  VideoPlaybackConfig config = base;
  if (continuation.hasLayout) {
    config.enableAscii = continuation.asciiRenderingEnabled;
  }
  return config;
}

class VideoSessionPresentation final
    : public playback_controller::ActivePresentation {
 public:
  VideoSessionPresentation(const playback_route::Route& route,
                           PlaybackSessionContinuationState& continuationState,
                           playback_controller::SessionCommands& commands,
                           TuiPlaybackPresenter::Services& services)
      : file_(route.target.file),
        continuationState_(continuationState),
        commands_(commands),
        services_(services),
        videoConfig_(sessionConfig(services.videoConfig, continuationState)),
        session_({file_, services.input, services.screen, services.baseStyle,
                  services.accentStyle, services.dimStyle,
                  services.progressEmptyStyle, services.progressFrameStyle,
                  services.progressStart, services.progressEnd, videoConfig_,
                  services.openFileRequests, &quitRequested_,
                  &services.systemControls, &services.notificationAreaControls,
                  [this](PlaybackTransportCommand command) {
                    return requestTransport(command);
                  },
                  [this](const std::vector<std::filesystem::path>& files) {
                    return requestOpenFiles(files);
                  },
                  &continuationState_, route.sessionIntent}) {}

  PlaybackSessionOpenOutcome open() { return session_.open(); }

  void run() override {
    session_.run();
    complete();
  }

  void complete() {
    if (quitRequested_) {
      services_.requestQuit();
    } else if (!successorAccepted_) {
      OpenFilesRequest request;
      if (services_.openFileRequests.poll(request)) {
        requestOpenFiles(request);
      }
    }

    services_.presentationFinished();
  }

 private:
  bool requestTransport(PlaybackTransportCommand command) {
    if (successorAccepted_) {
      return false;
    }
    const playback_controller::Direction direction =
        command == PlaybackTransportCommand::Previous
            ? playback_controller::Direction::Previous
            : playback_controller::Direction::Next;
    successorAccepted_ = commands_.transport(direction);
    return successorAccepted_;
  }

  bool requestOpenFiles(const std::vector<std::filesystem::path>& files) {
    OpenFilesRequest request;
    request.files = files;
    return requestOpenFiles(request);
  }

  bool requestOpenFiles(const OpenFilesRequest& request) {
    if (successorAccepted_) {
      return false;
    }
    successorAccepted_ = services_.requestOpenFiles(request);
    return successorAccepted_;
  }

  std::filesystem::path file_;
  PlaybackSessionContinuationState& continuationState_;
  playback_controller::SessionCommands& commands_;
  TuiPlaybackPresenter::Services& services_;
  VideoPlaybackConfig videoConfig_;
  bool quitRequested_ = false;
  bool successorAccepted_ = false;
  PlaybackSession session_;
};

}  // namespace

TuiPlaybackPresenter::TuiPlaybackPresenter(Services services)
    : services_(std::move(services)) {}

playback_controller::PresentationOpenResult TuiPlaybackPresenter::open(
    const playback_route::Route& route,
    playback_controller::SessionCommands& commands) {
  if (route.videoContinuation) {
    continuationState_ = *route.videoContinuation;
  }
  services_.applyAudioPictureInPicturePlan(route.audioPictureInPicture);

  const PlaybackTarget& target = route.target;
  if (target.trackIndex >= 0) {
    return services_.startAudio(target.file, target.trackIndex)
               ? playback_controller::PresentationOpenResult(
                     playback_controller::PresentationStarted{})
               : playback_controller::PresentationOpenResult(
                     playback_controller::PresentationRejected{});
  }
  if (isSupportedImageExt(target.file)) {
    return playback_controller::PresentationRejected{};
  }
  if (!isSupportedVideoExt(target.file)) {
    return services_.startAudio(target.file, 0)
               ? playback_controller::PresentationOpenResult(
                     playback_controller::PresentationStarted{})
               : playback_controller::PresentationOpenResult(
                     playback_controller::PresentationRejected{});
  }

  auto presentation = std::make_unique<VideoSessionPresentation>(
      route, continuationState_, commands, services_);
  switch (presentation->open()) {
    case PlaybackSessionOpenOutcome::Ready:
      return playback_controller::PresentationSession{std::move(presentation)};
    case PlaybackSessionOpenOutcome::AudioFallbackRequested:
      presentation.reset();
      return services_.startAudio(target.file, 0)
                 ? playback_controller::PresentationOpenResult(
                       playback_controller::PresentationStarted{})
                 : playback_controller::PresentationOpenResult(
                       playback_controller::PresentationRejected{});
    case PlaybackSessionOpenOutcome::HandledWithoutPlayback:
      presentation->complete();
      return playback_controller::PresentationHandled{};
  }
  return playback_controller::PresentationRejected{};
}
