#include "playback_coordinator.h"

#include <optional>
#include <utility>

#include "core/open_file_requests.h"
#include "media_formats.h"
#include "playback/control/transport.h"
#include "playback/session/session.h"
#include "playback/video/playback.h"

namespace {

class DrivingScope {
 public:
  explicit DrivingScope(bool& driving) : driving_(driving) { driving_ = true; }
  ~DrivingScope() { driving_ = false; }

  DrivingScope(const DrivingScope&) = delete;
  DrivingScope& operator=(const DrivingScope&) = delete;

 private:
  bool& driving_;
};

struct PresentationResult {
  std::optional<playback_queue::Queue::PreparedActivation> nextActivation;
};

VideoPlaybackConfig sessionConfig(
    const VideoPlaybackConfig& base,
    const PlaybackSessionContinuationState& continuation) {
  VideoPlaybackConfig config = base;
  if (continuation.hasLayout) {
    config.enableAscii = continuation.asciiRenderingEnabled;
  }
  return config;
}

playback_queue::Direction transportDirection(PlaybackTransportCommand command) {
  return command == PlaybackTransportCommand::Previous
             ? playback_queue::Direction::Previous
             : playback_queue::Direction::Next;
}

}  // namespace

struct TuiPlaybackCoordinator::Impl {
  explicit Impl(Services services) : services(std::move(services)) {}

  bool start(playback_route::Route route, playback_queue::Source source) {
    std::optional<playback_queue::Queue::PreparedActivation> activation =
        services.queue.prepareStart(std::move(route), std::move(source));
    return activation && drive(std::move(*activation));
  }

  bool transport(playback_queue::Direction direction) {
    std::optional<playback_queue::Queue::PreparedActivation> activation =
        services.queue.prepareTransport(direction);
    return activation && drive(std::move(*activation));
  }

  bool drive(playback_queue::Queue::PreparedActivation initialActivation) {
    if (driving) {
      return false;
    }

    DrivingScope drivingScope(driving);
    std::optional<playback_queue::Queue::PreparedActivation> activation(
        std::move(initialActivation));
    while (activation) {
      std::optional<PresentationResult> result =
          present(std::move(*activation));
      if (!result) {
        return false;
      }
      activation = std::move(result->nextActivation);
    }
    return true;
  }

  std::optional<PresentationResult> present(
      playback_queue::Queue::PreparedActivation activation) {
    const playback_route::Route& route = activation.route();
    if (route.videoContinuation) {
      continuationState = *route.videoContinuation;
    }
    services.applyAudioPictureInPicturePlan(route.audioPictureInPicture);

    const PlaybackTarget target = route.target;
    if (target.trackIndex >= 0) {
      if (!services.startAudio(target.file, target.trackIndex)) {
        return std::nullopt;
      }
      services.queue.commit(std::move(activation));
      return PresentationResult{};
    }
    if (isSupportedImageExt(target.file)) {
      return std::nullopt;
    }
    if (!isSupportedVideoExt(target.file)) {
      if (!services.startAudio(target.file, 0)) {
        return std::nullopt;
      }
      services.queue.commit(std::move(activation));
      return PresentationResult{};
    }

    bool quitRequested = false;
    bool sessionActive = false;
    bool successorAccepted = false;
    std::optional<playback_queue::Queue::PreparedActivation> pendingTransport;
    auto requestTransport = [&](PlaybackTransportCommand command) {
      if (!sessionActive || successorAccepted) {
        return false;
      }
      pendingTransport =
          services.queue.prepareTransport(transportDirection(command));
      successorAccepted = pendingTransport.has_value();
      return successorAccepted;
    };
    auto requestOpenFiles = [&](const OpenFilesRequest& request) {
      if (successorAccepted) {
        return false;
      }
      successorAccepted = services.requestOpenFiles(request);
      return successorAccepted;
    };
    auto requestDroppedFiles =
        [&](const std::vector<std::filesystem::path>& files) {
          OpenFilesRequest request;
          request.files = files;
          return requestOpenFiles(request);
        };

    const VideoPlaybackConfig videoConfig =
        sessionConfig(services.videoConfig, continuationState);
    std::optional<PlaybackSession> session;
    session.emplace(PlaybackSession::Args{
        target.file, services.input, services.screen, services.baseStyle,
        services.accentStyle, services.dimStyle, services.progressEmptyStyle,
        services.progressFrameStyle, services.progressStart,
        services.progressEnd, videoConfig, services.openFileRequests,
        &quitRequested, &services.systemControls,
        &services.notificationAreaControls, requestTransport,
        requestDroppedFiles, &continuationState, route.sessionIntent});

    const PlaybackSessionOpenOutcome openOutcome = session->open();
    if (openOutcome == PlaybackSessionOpenOutcome::Ready) {
      services.queue.commit(std::move(activation));
      sessionActive = true;
      session->run();
      sessionActive = false;
    } else if (openOutcome ==
               PlaybackSessionOpenOutcome::AudioFallbackRequested) {
      session.reset();
      if (!services.startAudio(target.file, 0)) {
        return std::nullopt;
      }
      services.queue.commit(std::move(activation));
      return PresentationResult{};
    }

    if (quitRequested) {
      pendingTransport.reset();
      services.requestQuit();
    } else if (!successorAccepted) {
      OpenFilesRequest request;
      if (services.openFileRequests.poll(request)) {
        requestOpenFiles(request);
      }
    }
    services.presentationFinished();
    return PresentationResult{std::move(pendingTransport)};
  }

  Services services;
  PlaybackSessionContinuationState continuationState;
  bool driving = false;
};

TuiPlaybackCoordinator::TuiPlaybackCoordinator(Services services)
    : impl_(std::make_unique<Impl>(std::move(services))) {}

TuiPlaybackCoordinator::~TuiPlaybackCoordinator() = default;

bool TuiPlaybackCoordinator::start(playback_route::Route route,
                                   playback_queue::Source source) {
  return impl_->start(std::move(route), std::move(source));
}

bool TuiPlaybackCoordinator::transport(playback_queue::Direction direction) {
  return impl_->transport(direction);
}
