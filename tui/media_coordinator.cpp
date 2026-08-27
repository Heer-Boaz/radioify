#include "tui/media_coordinator.h"

#include <cstdint>
#include <utility>
#include <variant>

#include "app/media_processing_coordinator.h"
#include "audio/audioplayback.h"
#include "audio/media_formats.h"
#include "core/path_identity.h"
#include "playback/target.h"
#include "tui/image_viewer.h"
#include "tui/media_activation_plan.h"

namespace {

enum class MediaCommandFailureKind : std::uint8_t {
  Busy,
  Unsupported,
  QueueUnavailable,
  PlaybackFailed,
  NavigationFailed,
};

struct MediaCommandFailure {
  MediaCommandFailureKind kind;
  std::string message;
};

class MediaCommandResult {
 public:
  enum class Status : std::uint8_t {
    Applied,
    Deferred,
    HandledWithoutPlayback,
    Rejected,
  };

  static MediaCommandResult applied() {
    return MediaCommandResult(Status::Applied);
  }
  static MediaCommandResult deferred() {
    return MediaCommandResult(Status::Deferred);
  }
  static MediaCommandResult handledWithoutPlayback() {
    return MediaCommandResult(Status::HandledWithoutPlayback);
  }
  static MediaCommandResult rejected(MediaCommandFailure failure) {
    return MediaCommandResult(std::move(failure));
  }

  bool accepted() const { return status_ != Status::Rejected; }
  const MediaCommandFailure* failure() const {
    return failure_ ? &*failure_ : nullptr;
  }

 private:
  explicit MediaCommandResult(Status status) : status_(status) {}
  explicit MediaCommandResult(MediaCommandFailure failure)
      : status_(Status::Rejected), failure_(std::move(failure)) {}

  Status status_;
  std::optional<MediaCommandFailure> failure_;
};

}  // namespace

struct TuiMediaCoordinator::Impl {
  explicit Impl(Services services) : services_(std::move(services)) {}

  MediaCommandResult startPlayback(playback_route::Route route,
                                   playback_queue::Source source) {
    std::optional<playback_queue::Queue::PreparedActivation> activation =
        services_.queue.prepareStart(std::move(route), std::move(source));
    if (!activation) {
      return reject(MediaCommandFailureKind::QueueUnavailable,
                    "Unable to prepare the playback queue.");
    }
    return submit(PreparedPlayback{std::move(*activation)});
  }

  MediaCommandResult startFiles(
      playback_route::Route route,
      const std::vector<std::filesystem::path>& files) {
    CommandBuildResult command = mediaCommandFromFiles(std::move(route), files);
    if (auto* failure = std::get_if<MediaCommandFailure>(&command)) {
      return reject(std::move(*failure));
    }
    return submit(std::move(std::get<Command>(command)));
  }

  MediaCommandResult startDroppedFiles(
      const std::vector<std::filesystem::path>& files,
      const WindowPlacementState* sourcePlacement,
      std::optional<PlaybackPresentationState> videoPresentation) {
    std::optional<playback_route::Route> route =
        playback_route::resolveDroppedTarget(files, sourcePlacement,
                                             videoPresentation);
    if (!route) {
      return reject(MediaCommandFailureKind::Unsupported,
                    "No supported media item was found in the drop request.");
    }
    return startFiles(std::move(*route), files);
  }

  MediaCommandResult openFiles(const OpenFilesRequest& request) {
    CommandBuildResult command = commandFromOpenFiles(request);
    if (auto* failure = std::get_if<MediaCommandFailure>(&command)) {
      return reject(std::move(*failure));
    }
    return submit(std::move(std::get<Command>(command)));
  }

  MediaCommandResult transport(playback_queue::Direction direction) {
    std::optional<playback_queue::Queue::PreparedActivation> activation =
        services_.queue.prepareTransport(direction);
    if (!activation) {
      return reject(MediaCommandFailureKind::QueueUnavailable, {});
    }
    return submit(PreparedPlayback{std::move(*activation)});
  }

  bool pump() {
    if (videoSession_) {
      if (std::optional<PlaybackSessionCompletion> completion =
              videoSession_->pump()) {
        finishVideoSession(std::move(*completion));
        drainPendingCommands();
        return true;
      }
    }
    return false;
  }

  bool videoActive() const { return videoSession_.has_value(); }

  PlaybackShellTerminalRole terminalRole() const {
    return videoSession_ ? videoSession_->terminalRole()
                         : PlaybackShellTerminalRole::Browser;
  }

  static std::optional<PlaybackTarget> playbackTargetForAudio(
      const std::optional<AudioPlaybackSource>& source) {
    if (!source) return std::nullopt;
    if (source->trackIndex) {
      if (std::optional<PlaybackTarget> trackTarget =
              playbackTrackTarget(source->file, *source->trackIndex)) {
        return trackTarget;
      }
    }
    return playbackFileTarget(source->file);
  }

  std::optional<VideoSnapshot> videoSnapshot() const {
    if (!videoSession_) return std::nullopt;
    return VideoSnapshot{videoSession_->controlState(),
                         videoSession_->presentationState()};
  }

  std::vector<NativeWaitHandle> waitHandles() const {
    return videoSession_ ? videoSession_->activityWaitHandles()
                         : std::vector<NativeWaitHandle>{};
  }

  wake_schedule::Deadline nextWakeDeadline() const {
    return videoSession_ ? videoSession_->nextWakeDeadline() : std::nullopt;
  }

  bool capturesBrowserInput() const {
    return videoSession_ && videoSession_->capturesBrowserInput();
  }

  bool handleVideoInputEvent(const InputEvent& event) {
    return videoSession_ && videoSession_->handleInputEvent(event);
  }

  bool handleControlCommand(PlaybackControlCommand command) {
    if (videoSession_) return videoSession_->handleControlCommand(command);

    const AudioPlaybackSnapshot audio = audioGetPlaybackSnapshot();
    const bool hasAudioTarget =
        playbackTargetForAudio(audio.source).has_value();
    switch (command) {
      case PlaybackControlCommand::Play:
        if (!hasAudioTarget) return false;
        audioPlay();
        return true;
      case PlaybackControlCommand::Pause:
        if (!hasAudioTarget) return false;
        audioPause();
        return true;
      case PlaybackControlCommand::TogglePause:
        if (!hasAudioTarget) return false;
        audioTogglePause();
        return true;
      case PlaybackControlCommand::Stop:
        if (!audio.ready) return false;
        audioStop();
        return true;
      case PlaybackControlCommand::Previous:
        if (!hasAudioTarget) return false;
        return transport(playback_queue::Direction::Previous).accepted();
      case PlaybackControlCommand::Next:
        if (!hasAudioTarget) return false;
        return transport(playback_queue::Direction::Next).accepted();
    }
    return false;
  }

  bool seekToRatio(double ratio) {
    if (videoSession_) return videoSession_->seekToRatio(ratio);
    if (!audioGetPlaybackSnapshot().ready) return false;
    audioSeekToRatio(ratio);
    return true;
  }

  bool toggleWindowPresentation() {
    return videoSession_ && videoSession_->toggleWindowPresentation();
  }

  bool togglePictureInPicture() {
    return videoSession_ && videoSession_->togglePictureInPicture();
  }

  bool toggleFullscreen() {
    return videoSession_ && videoSession_->toggleFullscreen();
  }

  bool activateVideoPresentation() {
    return videoSession_ && videoSession_->activatePresentation();
  }

  void handleMediaTaskCompletion(
      const media_processing::TaskCompletion& completion,
      std::string presentationStatus) {
    if (!videoSession_ || !videoTarget_ ||
        !samePath(playbackTargetFile(*videoTarget_), completion.sourceFile)) {
      return;
    }
    switch (completion.kind) {
      case media_processing::TaskKind::SubtitleGeneration:
        videoSession_->subtitleGenerationFinished(
            completion.outputFile, completion.succeeded(),
            std::move(presentationStatus));
        return;
      case media_processing::TaskKind::AudioSeparation:
        videoSession_->mediaTaskFinished(std::move(presentationStatus));
        return;
      case media_processing::TaskKind::MelodyAnalysis:
      case media_processing::TaskKind::LoopSplit:
        return;
    }
  }

  void requestQuit() {
    if (videoSession_) {
      videoSession_->requestQuit();
    } else if (services_.callbacks.requestQuit) {
      services_.callbacks.requestQuit();
    }
  }

 private:
  struct PreparedPlayback {
    playback_queue::Queue::PreparedActivation activation;
  };

  struct Quit {};

  using Command = std::variant<PreparedPlayback,
                               tui_media_activation::ShowImages,
                               tui_media_activation::OpenDirectory, Quit>;
  using CommandBuildResult = std::variant<Command, MediaCommandFailure>;

  class DriveScope {
   public:
    explicit DriveScope(bool& driving) : driving_(driving) { driving_ = true; }
    ~DriveScope() { driving_ = false; }

    DriveScope(const DriveScope&) = delete;
    DriveScope& operator=(const DriveScope&) = delete;

   private:
    bool& driving_;
  };

  CommandBuildResult mediaCommandFromFiles(
      playback_route::Route route,
      const std::vector<std::filesystem::path>& files) const {
    return commandFromPlan(
        tui_media_activation::planFiles(std::move(route), files));
  }

  CommandBuildResult commandFromPlan(
      tui_media_activation::QueueFiles queuedFiles) const {
    std::optional<playback_queue::Queue::PreparedActivation> activation =
        services_.queue.prepareStart(
            std::move(queuedFiles.route),
            playback_queue::sourceFromFiles(queuedFiles.files));
    if (!activation) {
      return MediaCommandFailure{
          MediaCommandFailureKind::QueueUnavailable,
          "Unable to prepare the playback queue."};
    }
    return Command(PreparedPlayback{std::move(*activation)});
  }

  CommandBuildResult commandFromPlan(
      tui_media_activation::ShowImages showImages) const {
    return Command(std::move(showImages));
  }

  CommandBuildResult commandFromPlan(
      tui_media_activation::OpenDirectory openDirectory) const {
    return Command(std::move(openDirectory));
  }

  CommandBuildResult commandFromPlan(
      tui_media_activation::Failure failure) const {
    return MediaCommandFailure{MediaCommandFailureKind::Unsupported,
                               std::move(failure.message)};
  }

  CommandBuildResult commandFromPlan(
      tui_media_activation::Plan plan) const {
    return std::visit(
        [this](auto value) -> CommandBuildResult {
          return commandFromPlan(std::move(value));
        },
        std::move(plan));
  }

  CommandBuildResult commandFromOpenFiles(
      const OpenFilesRequest& request) const {
    const auto defaultPresentation =
        services_.videoConfig.enableAscii
            ? tui_media_activation::DefaultVideoPresentation::TerminalAscii
            : tui_media_activation::DefaultVideoPresentation::
                  NativeWindowedFramebuffer;
    return commandFromPlan(
        tui_media_activation::planOpenFiles(request, defaultPresentation));
  }

  MediaCommandResult enqueueOpenFiles(const OpenFilesRequest& request) {
    if (pendingCommand_) return reject(MediaCommandFailureKind::Busy, {});
    CommandBuildResult command = commandFromOpenFiles(request);
    if (auto* failure = std::get_if<MediaCommandFailure>(&command)) {
      return reject(std::move(*failure));
    }
    pendingCommand_.emplace(std::move(std::get<Command>(command)));
    clearCommandError();
    return MediaCommandResult::deferred();
  }

  void enqueueQuit() {
    if (!pendingCommand_) pendingCommand_.emplace(Quit{});
  }

  MediaCommandResult requestVideoHandoff(Command command) {
    if (!videoSession_ || pendingCommand_ || handoffCommand_) {
      return reject(MediaCommandFailureKind::Busy, {});
    }
    handoffCommand_.emplace(std::move(command));
    const bool requested = videoSession_->requestHandoff([this](bool accepted) {
      if (!handoffCommand_) return;
      if (accepted && !pendingCommand_) {
        pendingCommand_.emplace(std::move(*handoffCommand_));
      }
      handoffCommand_.reset();
    });
    if (!requested) {
      handoffCommand_.reset();
      return reject(MediaCommandFailureKind::Busy, {});
    }
    clearCommandError();
    return MediaCommandResult::deferred();
  }

  MediaCommandResult submit(Command command) {
    if (videoSession_) return requestVideoHandoff(std::move(command));
    if (driving_) {
      if (pendingCommand_) return reject(MediaCommandFailureKind::Busy, {});
      pendingCommand_.emplace(std::move(command));
      clearCommandError();
      return MediaCommandResult::deferred();
    }
    return drive(std::move(command));
  }

  MediaCommandResult drive(Command initialCommand) {
    if (driving_) return reject(MediaCommandFailureKind::Busy, {});

    DriveScope driveScope(driving_);
    std::optional<Command> command(std::move(initialCommand));
    MediaCommandResult result = MediaCommandResult::handledWithoutPlayback();
    while (command) {
      result = dispatch(std::move(*command));
      if (!result.accepted()) {
        pendingCommand_.reset();
        publishFailure(result);
        return result;
      }
      if (videoSession_) break;
      command = std::exchange(pendingCommand_, std::nullopt);
    }
    clearCommandError();
    return result;
  }

  std::optional<MediaCommandResult> drainPendingCommands() {
    if (videoSession_ || driving_ || !pendingCommand_) return std::nullopt;
    Command command = std::move(*pendingCommand_);
    pendingCommand_.reset();
    return drive(std::move(command));
  }

  static VideoPlaybackConfig sessionConfig(
      const VideoPlaybackConfig& base,
      const PlaybackSessionContinuationState& continuation) {
    VideoPlaybackConfig config = base;
    if (continuation.presentation) {
      config.enableAscii = continuation.presentation->usesAsciiGrid();
    }
    return config;
  }

  static playback_queue::Direction transportDirection(
      PlaybackTransportCommand command) {
    return command == PlaybackTransportCommand::Previous
               ? playback_queue::Direction::Previous
               : playback_queue::Direction::Next;
  }

  MediaCommandResult presentPlayback(
      playback_queue::Queue::PreparedActivation activation) {
    const playback_route::Route& route = activation.route();
    if (route.videoContinuation) {
      continuationState_ = *route.videoContinuation;
    }
    if (services_.callbacks.applyAudioPictureInPicturePlan) {
      services_.callbacks.applyAudioPictureInPicturePlan(
          route.audioPictureInPicture);
    }

    const PlaybackTarget target = route.target;
    const std::filesystem::path& targetFile = playbackTargetFile(target);
    if (const std::optional<int> trackIndex =
            playbackTargetTrackIndex(target)) {
      if (!services_.callbacks.startAudio ||
          !services_.callbacks.startAudio(targetFile, *trackIndex)) {
        return MediaCommandResult::rejected(
            {MediaCommandFailureKind::PlaybackFailed, {}});
      }
      services_.queue.commit(std::move(activation));
      return MediaCommandResult::applied();
    }
    if (isSupportedImageExt(targetFile)) {
      return MediaCommandResult::rejected(
          {MediaCommandFailureKind::Unsupported,
           "The image request did not contain an image sequence."});
    }
    if (!isSupportedVideoExt(targetFile)) {
      if (!services_.callbacks.startAudio ||
          !services_.callbacks.startAudio(targetFile, 0)) {
        return MediaCommandResult::rejected(
            {MediaCommandFailureKind::PlaybackFailed, {}});
      }
      services_.queue.commit(std::move(activation));
      return MediaCommandResult::applied();
    }

    auto requestTransport = [this](PlaybackTransportCommand command) {
      if (!videoSession_ || pendingCommand_) return false;
      std::optional<playback_queue::Queue::PreparedActivation> successor =
          services_.queue.prepareTransport(transportDirection(command));
      if (!successor) return false;
      pendingCommand_.emplace(PreparedPlayback{std::move(*successor)});
      return true;
    };
    auto requestDroppedFiles =
        [this](const std::vector<std::filesystem::path>& files) {
          OpenFilesRequest request;
          request.files = files;
          return enqueueOpenFiles(request).accepted();
        };

    PlaybackSession::Request sessionRequest;
    sessionRequest.file = targetFile;
    sessionRequest.config = sessionConfig(services_.videoConfig,
                                          continuationState_);
    sessionRequest.continuityState = continuationState_;
    sessionRequest.sessionIntent = route.sessionIntent;
    sessionRequest.requestTransportCommand = std::move(requestTransport);
    sessionRequest.requestOpenFiles = std::move(requestDroppedFiles);
    sessionRequest.mediaProcessingActions = services_.mediaProcessingActions;
    sessionRequest.activateBrowserSurface =
        services_.callbacks.activateBrowserSurface;
    videoSession_.emplace(std::move(sessionRequest),
                          services_.sessionDependencies);

    const PlaybackSessionOpenOutcome openOutcome = videoSession_->open();
    if (openOutcome == PlaybackSessionOpenOutcome::Ready) {
      services_.queue.commit(std::move(activation));
      videoTarget_ = target;
      presentationFinished();
      return MediaCommandResult::applied();
    }
    if (openOutcome == PlaybackSessionOpenOutcome::AudioFallbackRequested) {
      videoSession_.reset();
      if (!services_.callbacks.startAudio ||
          !services_.callbacks.startAudio(targetFile, 0)) {
        return MediaCommandResult::rejected(
            {MediaCommandFailureKind::PlaybackFailed, {}});
      }
      services_.queue.commit(std::move(activation));
      return MediaCommandResult::applied();
    }
    if (openOutcome ==
        PlaybackSessionOpenOutcome::QuitApplicationRequested) {
      videoSession_.reset();
      enqueueQuit();
      presentationFinished();
      return MediaCommandResult::handledWithoutPlayback();
    }
    videoSession_.reset();
    presentationFinished();
    return MediaCommandResult::handledWithoutPlayback();
  }

  void finishVideoSession(PlaybackSessionCompletion completion) {
    continuationState_ = std::move(completion.continuityState);
    videoSession_.reset();
    videoTarget_.reset();
    if (completion.intent == PlaybackSessionExitIntent::QuitApplication) {
      enqueueQuit();
    }
    presentationFinished();
  }

  MediaCommandResult dispatch(PreparedPlayback playback) {
    return presentPlayback(std::move(playback.activation));
  }

  MediaCommandResult dispatch(tui_media_activation::ShowImages image) {
    if (services_.callbacks.applyAudioPictureInPicturePlan) {
      services_.callbacks.applyAudioPictureInPicturePlan(
          image.route.audioPictureInPicture);
    }
    const PlaybackSession::Dependencies& dependencies =
        services_.sessionDependencies;
    const image_viewer::Exit exit = image_viewer::run(
        std::move(image.sequence), dependencies.input, dependencies.screen,
        dependencies.appearance.baseStyle,
        dependencies.appearance.accentStyle,
        dependencies.appearance.dimStyle, services_.openFileRequests,
        [this](const OpenFilesRequest& request) {
          return enqueueOpenFiles(request).accepted();
        });
    if (exit == image_viewer::Exit::QuitRequested) enqueueQuit();
    presentationFinished();
    return MediaCommandResult::applied();
  }

  MediaCommandResult dispatch(
      tui_media_activation::OpenDirectory directory) {
    if (!services_.callbacks.openBrowserDirectory ||
        !services_.callbacks.openBrowserDirectory(directory.path)) {
      return MediaCommandResult::rejected(
          {MediaCommandFailureKind::NavigationFailed,
           "Unable to open the requested folder."});
    }
    return MediaCommandResult::applied();
  }

  MediaCommandResult dispatch(Quit) {
    if (services_.callbacks.requestQuit) services_.callbacks.requestQuit();
    return MediaCommandResult::applied();
  }

  MediaCommandResult dispatch(Command command) {
    return std::visit(
        [this](auto action) { return dispatch(std::move(action)); },
        std::move(command));
  }

  MediaCommandResult reject(MediaCommandFailureKind kind,
                            std::string message) {
    return reject(MediaCommandFailure{kind, std::move(message)});
  }

  MediaCommandResult reject(MediaCommandFailure failure) {
    MediaCommandResult result =
        MediaCommandResult::rejected(std::move(failure));
    publishFailure(result);
    return result;
  }

  void publishFailure(const MediaCommandResult& result) {
    const MediaCommandFailure* failure = result.failure();
    if (failure && !failure->message.empty() &&
        services_.callbacks.setCommandError) {
      services_.callbacks.setCommandError(failure->message);
    }
  }

  void clearCommandError() {
    if (services_.callbacks.setCommandError) {
      services_.callbacks.setCommandError({});
    }
  }

  void presentationFinished() {
    if (services_.callbacks.presentationFinished) {
      services_.callbacks.presentationFinished();
    }
  }

  Services services_;
  PlaybackSessionContinuationState continuationState_;
  std::optional<PlaybackSession> videoSession_;
  std::optional<PlaybackTarget> videoTarget_;
  std::optional<Command> pendingCommand_;
  std::optional<Command> handoffCommand_;
  bool driving_ = false;
};

TuiMediaCoordinator::TuiMediaCoordinator(Services services)
    : impl_(std::make_unique<Impl>(std::move(services))) {}

TuiMediaCoordinator::~TuiMediaCoordinator() = default;

bool TuiMediaCoordinator::startPlayback(playback_route::Route route,
                                        playback_queue::Source source) {
  return impl_->startPlayback(std::move(route), std::move(source)).accepted();
}

bool TuiMediaCoordinator::startFiles(
    playback_route::Route route,
    const std::vector<std::filesystem::path>& files) {
  return impl_->startFiles(std::move(route), files).accepted();
}

bool TuiMediaCoordinator::startDroppedFiles(
    const std::vector<std::filesystem::path>& files,
    const WindowPlacementState* sourcePlacement,
    std::optional<PlaybackPresentationState> videoPresentation) {
  return impl_->startDroppedFiles(files, sourcePlacement, videoPresentation)
      .accepted();
}

bool TuiMediaCoordinator::openFiles(const OpenFilesRequest& request) {
  return impl_->openFiles(request).accepted();
}

bool TuiMediaCoordinator::pump() {
  return impl_->pump();
}

void TuiMediaCoordinator::handleMediaTaskCompletion(
    const media_processing::TaskCompletion& completion,
    std::string presentationStatus) {
  impl_->handleMediaTaskCompletion(completion,
                                   std::move(presentationStatus));
}

bool TuiMediaCoordinator::videoActive() const { return impl_->videoActive(); }

PlaybackShellTerminalRole TuiMediaCoordinator::terminalRole() const {
  return impl_->terminalRole();
}

std::optional<TuiMediaCoordinator::VideoSnapshot>
TuiMediaCoordinator::videoSnapshot() const {
  return impl_->videoSnapshot();
}

std::vector<NativeWaitHandle> TuiMediaCoordinator::waitHandles() const {
  return impl_->waitHandles();
}

wake_schedule::Deadline TuiMediaCoordinator::nextWakeDeadline() const {
  return impl_->nextWakeDeadline();
}

bool TuiMediaCoordinator::capturesBrowserInput() const {
  return impl_->capturesBrowserInput();
}

bool TuiMediaCoordinator::handleVideoInputEvent(const InputEvent& event) {
  return impl_->handleVideoInputEvent(event);
}

bool TuiMediaCoordinator::handleControlCommand(
    PlaybackControlCommand command) {
  return impl_->handleControlCommand(command);
}

bool TuiMediaCoordinator::seekToRatio(double ratio) {
  return impl_->seekToRatio(ratio);
}

bool TuiMediaCoordinator::toggleWindowPresentation() {
  return impl_->toggleWindowPresentation();
}

bool TuiMediaCoordinator::togglePictureInPicture() {
  return impl_->togglePictureInPicture();
}

bool TuiMediaCoordinator::toggleFullscreen() {
  return impl_->toggleFullscreen();
}

bool TuiMediaCoordinator::activateVideoPresentation() {
  return impl_->activateVideoPresentation();
}

void TuiMediaCoordinator::requestQuit() { impl_->requestQuit(); }
