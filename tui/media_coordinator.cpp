#include "tui/media_coordinator.h"

#include <cstdint>
#include <utility>
#include <variant>

#include "app/media_processing_coordinator.h"
#include "audio/audioplayback.h"
#include "audio/media_formats.h"
#include "core/path_identity.h"
#include "core/wakeable_mailbox.h"
#include "playback/target.h"
#include "tui/media_activation_plan.h"

namespace {

enum class MediaCommandFailureKind : std::uint8_t {
  Busy,
  Unsupported,
  QueueUnavailable,
  PlaybackFailed,
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

  PollResult poll() {
    bool playbackChanged = false;
    if (videoSession_) {
      drainVideoSessionEvents();
      if (std::optional<PlaybackSessionCompletion> completion =
              videoSession_->pump()) {
        drainVideoSessionEvents();
        finishVideoSession(std::move(*completion));
        drainPendingCommands();
        playbackChanged = true;
      } else {
        drainVideoSessionEvents();
      }
    }
    return PollResult{playbackChanged, events_.drain()};
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
    std::vector<NativeWaitHandle> handles =
        videoSession_ ? videoSession_->activityWaitHandles()
                      : std::vector<NativeWaitHandle>{};
    if (NativeWaitHandle eventHandle = events_.nativeWaitHandle()) {
      handles.push_back(eventHandle);
    }
    return handles;
  }

  wake_schedule::Deadline nextWakeDeadline() const {
    return videoSession_ ? videoSession_->nextWakeDeadline() : std::nullopt;
  }

  bool capturesBrowserInput() const {
    return videoSession_ && videoSession_->capturesBrowserInput();
  }

  bool handleVideoInputEvent(const InputEvent& event) {
    if (!videoSession_) return false;
    const bool handled = videoSession_->handleInputEvent(event);
    drainVideoSessionEvents();
    return handled;
  }

  bool handleControlCommand(PlaybackControlCommand command) {
    if (videoSession_) {
      const bool handled = videoSession_->handleControlCommand(command);
      drainVideoSessionEvents();
      return handled;
    }

    const AudioPlaybackSnapshot audio = services_.audioPlayback.snapshot();
    const bool hasAudioTarget =
        playbackTargetForAudio(audio.source).has_value();
    switch (command) {
      case PlaybackControlCommand::Play:
        if (!hasAudioTarget) return false;
        services_.audioPlayback.play();
        return true;
      case PlaybackControlCommand::Pause:
        if (!hasAudioTarget) return false;
        services_.audioPlayback.pause();
        return true;
      case PlaybackControlCommand::TogglePause:
        if (!hasAudioTarget) return false;
        services_.audioPlayback.togglePause();
        return true;
      case PlaybackControlCommand::Stop:
        if (!audio.ready) return false;
        services_.audioPlayback.stop();
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
    if (!services_.audioPlayback.snapshot().ready) return false;
    services_.audioPlayback.seekToRatio(ratio);
    return true;
  }

  bool toggleWindowPresentation() {
    if (!videoSession_) return false;
    const bool handled = videoSession_->toggleWindowPresentation();
    drainVideoSessionEvents();
    return handled;
  }

  bool togglePictureInPicture() {
    if (!videoSession_) return false;
    const bool handled = videoSession_->togglePictureInPicture();
    drainVideoSessionEvents();
    return handled;
  }

  bool toggleFullscreen() {
    if (!videoSession_) return false;
    const bool handled = videoSession_->toggleFullscreen();
    drainVideoSessionEvents();
    return handled;
  }

  bool activateVideoPresentation() {
    if (!videoSession_) return false;
    const bool handled = videoSession_->activatePresentation();
    drainVideoSessionEvents();
    return handled;
  }

  void handleMediaTaskCompletion(
      const media_processing::TaskCompletion& completion) {
    if (!videoSession_ || !videoTarget_ ||
        !samePath(playbackTargetFile(*videoTarget_), completion.sourceFile)) {
      return;
    }
    if (std::optional<playback_media_processing::Completion> projected =
            media_processing::completionForPlayback(completion)) {
      videoSession_->mediaTaskFinished(*projected);
    }
  }

  void requestQuit() {
    if (videoSession_) {
      videoSession_->requestQuit();
    } else {
      publishEvent(QuitRequested{});
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

  bool acceptSessionHandoff(
      const playback_session_exit::HandoffRequest& request) {
    if (pendingCommand_) return false;

    if (const auto* transport =
            std::get_if<playback_session_exit::Transport>(&request.intent)) {
      std::optional<playback_queue::Queue::PreparedActivation> successor =
          services_.queue.prepareTransport(
              transportDirection(transport->command));
      if (!successor) return false;
      pendingCommand_.emplace(PreparedPlayback{std::move(*successor)});
      return true;
    }
    if (const auto* openFiles =
            std::get_if<playback_session_exit::OpenFiles>(&request.intent)) {
      OpenFilesRequest openRequest;
      openRequest.files = openFiles->files;
      return enqueueOpenFiles(openRequest).accepted();
    }
    if (!std::holds_alternative<playback_session_exit::ExternalHandoff>(
            request.intent) ||
        !handoffCommand_ || handoffRequestId_ != request.id) {
      return false;
    }
    pendingCommand_.emplace(std::move(*handoffCommand_));
    handoffCommand_.reset();
    handoffRequestId_.reset();
    return true;
  }

  void drainVideoSessionEvents() {
    if (!videoSession_) return;
    std::vector<playback_session::Event> sessionEvents =
        videoSession_->drainEvents();
    for (const playback_session::Event& event : sessionEvents) {
      if (const auto* request =
              std::get_if<playback_session_exit::HandoffRequest>(&event)) {
        const bool accepted = acceptSessionHandoff(*request);
        videoSession_->resolveHandoff(request->id, accepted);
        continue;
      }
      if (const auto* cancellation =
              std::get_if<playback_session_exit::HandoffCancellation>(
                  &event)) {
        if (handoffRequestId_ == cancellation->id) {
          handoffCommand_.reset();
          handoffRequestId_.reset();
        }
        continue;
      }
      publishEvent(ActivateBrowserSurface{});
    }
  }

  MediaCommandResult requestVideoHandoff(Command command) {
    if (!videoSession_ || pendingCommand_ || handoffCommand_ ||
        handoffRequestId_) {
      return reject(MediaCommandFailureKind::Busy, {});
    }
    handoffCommand_.emplace(std::move(command));
    const std::optional<playback_session_exit::RequestId> requestId =
        videoSession_->requestHandoff();
    if (!requestId) {
      handoffCommand_.reset();
      return reject(MediaCommandFailureKind::Busy, {});
    }
    handoffRequestId_ = requestId;
    drainVideoSessionEvents();
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
    publishEvent(ApplyAudioPictureInPicture{route.audioPictureInPicture});

    const PlaybackTarget target = route.target;
    const std::filesystem::path& targetFile = playbackTargetFile(target);
    if (const std::optional<int> trackIndex =
            playbackTargetTrackIndex(target)) {
      if (!services_.audioPlayback.startFile(targetFile, *trackIndex)) {
        publishEvent(AudioPlaybackFailed{targetFile});
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
      if (!services_.audioPlayback.startFile(targetFile, 0)) {
        publishEvent(AudioPlaybackFailed{targetFile});
        return MediaCommandResult::rejected(
            {MediaCommandFailureKind::PlaybackFailed, {}});
      }
      services_.queue.commit(std::move(activation));
      return MediaCommandResult::applied();
    }

    PlaybackSession::Request sessionRequest;
    sessionRequest.file = targetFile;
    sessionRequest.config = sessionConfig(services_.videoConfig,
                                          continuationState_);
    sessionRequest.continuityState = continuationState_;
    sessionRequest.sessionIntent = route.sessionIntent;
    sessionRequest.capabilities.transportHandoff = true;
    sessionRequest.capabilities.openFilesHandoff = true;
    sessionRequest.capabilities.browserSurfaceActivation = true;
    sessionRequest.mediaProcessingActions = services_.mediaProcessingActions;
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
      if (!services_.audioPlayback.startFile(targetFile, 0)) {
        publishEvent(AudioPlaybackFailed{targetFile});
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
    handoffCommand_.reset();
    handoffRequestId_.reset();
    if (completion.intent == PlaybackSessionExitIntent::QuitApplication) {
      enqueueQuit();
    }
    presentationFinished();
  }

  MediaCommandResult dispatch(PreparedPlayback playback) {
    return presentPlayback(std::move(playback.activation));
  }

  MediaCommandResult dispatch(tui_media_activation::ShowImages image) {
    publishEvent(ShowImages{image.route.audioPictureInPicture,
                            std::move(image.sequence)});
    return MediaCommandResult::applied();
  }

  MediaCommandResult dispatch(
      tui_media_activation::OpenDirectory directory) {
    publishEvent(OpenBrowserDirectory{std::move(directory.path)});
    return MediaCommandResult::applied();
  }

  MediaCommandResult dispatch(Quit) {
    publishEvent(QuitRequested{});
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
        failure->message != commandError_) {
      commandError_ = failure->message;
      publishEvent(CommandErrorChanged{commandError_});
    }
  }

  void clearCommandError() {
    if (commandError_.empty()) return;
    commandError_.clear();
    publishEvent(CommandErrorChanged{});
  }

  void presentationFinished() {
    publishEvent(PresentationFinished{});
  }

  void publishEvent(Event event) {
    events_.publish(std::move(event));
  }

  Services services_;
  PlaybackSessionContinuationState continuationState_;
  std::optional<PlaybackSession> videoSession_;
  std::optional<PlaybackTarget> videoTarget_;
  std::optional<Command> pendingCommand_;
  std::optional<Command> handoffCommand_;
  std::optional<playback_session_exit::RequestId> handoffRequestId_;
  std::string commandError_;
  WakeableMailbox<Event> events_;
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

TuiMediaCoordinator::PollResult TuiMediaCoordinator::poll() {
  return impl_->poll();
}

void TuiMediaCoordinator::handleMediaTaskCompletion(
    const media_processing::TaskCompletion& completion) {
  impl_->handleMediaTaskCompletion(completion);
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
