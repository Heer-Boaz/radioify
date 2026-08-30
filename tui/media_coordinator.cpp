#include "tui/media_coordinator.h"

#include <cassert>
#include <cstdint>
#include <utility>
#include <variant>

#include "app/media_processing_coordinator.h"
#include "app/playback_control_router.h"
#include "app/video_handoff_controller.h"
#include "app/video_session_host.h"
#include "audio/media_formats.h"
#include "core/path_identity.h"
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
  bool isDeferred() const { return status_ == Status::Deferred; }
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
  explicit Impl(Services services)
      : services_(std::move(services)),
        playbackControl_(services_.audioPlayback),
        videoSessions_(std::move(services_.createVideoSession)) {}

  audio_playback::Session& audioPlayback() const {
    return services_.audioPlayback;
  }

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
    if (videoSessions_.empty() && pendingCommand_) {
      drainPendingCommands();
      playbackChanged = !videoSessions_.empty();
    }
    if (!videoSessions_.empty()) {
      if (videoSessions_.opening()) {
        if (std::optional<VideoSessions::OpenFinished> opened =
                videoSessions_.pumpOpen()) {
          finishVideoOpen(std::move(*opened));
          drainPendingCommands();
          playbackChanged = true;
        }
      } else {
        drainVideoSessionEvents();
        resumeDeferredVideoHandoff();
        if (videoSessions_.pumpPlayback()) {
          drainVideoSessionEvents();
          assert(videoSessions_.completionPending());
          playbackControl_.endSession();
          std::optional<VideoSessions::SessionFinished> finished =
              videoSessions_.takeCompletion();
          assert(finished);
          finishVideoSession(std::move(*finished));
          drainPendingCommands();
          playbackChanged = true;
        } else {
          drainVideoSessionEvents();
          resumeDeferredVideoHandoff();
        }
      }
    }
    synchronizeMediaTaskActivity();
    std::vector<Event> events;
    events.swap(events_);
    return PollResult{playbackChanged, std::move(events)};
  }

  bool videoReady() const {
    return videoSessions_.ready();
  }

  PlaybackControlSessionId controlSessionId() const {
    return playbackControl_.sessionId();
  }

  PlaybackShellTerminalRole terminalRole() const {
    return videoSessions_.terminalRole().value_or(
        PlaybackShellTerminalRole::Browser);
  }

  application_playback::VideoSessionRef videoSessionRef() {
    return videoSessions_.session();
  }

  application_playback::ConstVideoSessionRef videoSessionRef() const {
    return videoSessions_.session();
  }

  std::optional<VideoSnapshot> videoSnapshot() const {
    std::optional<PlaybackControlState> control =
        playbackControl_.videoControlState(videoSessionRef());
    std::optional<PlaybackPresentationState> presentation =
        videoSessions_.presentationState();
    if (!control || !presentation) return std::nullopt;
    return VideoSnapshot{std::move(*control), std::move(*presentation)};
  }

  std::optional<playback_session::TransitionSnapshot>
  videoTransitionSnapshot() const {
    return videoSessions_.transitionSnapshot();
  }

  std::vector<NativeWaitHandle> waitHandles() const {
    return videoSessions_.activityWaitHandles();
  }

  wake_schedule::Deadline nextWakeDeadline() const {
    wake_schedule::Deadline deadline = videoSessions_.nextWakeDeadline();
    if (!events_.empty()) {
      wake_schedule::include(deadline, wake_schedule::Clock::now());
    }
    return deadline;
  }

  bool capturesBrowserInput() const {
    return videoSessions_.capturesBrowserInput();
  }

  bool canAcceptExternalMediaChange() const {
    const bool sessionCanHandoff =
        videoSessions_.empty() || videoSessions_.ready();
    return sessionCanHandoff && !driving_ && !pendingCommand_ &&
           !pendingAudioFallback_ && externalHandoff_.empty();
  }

  void setExternalInputModal(bool modal) {
    videoSessions_.setExternalInputModal(modal);
  }

  bool handleVideoInputEvent(const InputEvent& event) {
    const bool handled = videoSessions_.handleInputEvent(event);
    drainVideoSessionEvents();
    resumeDeferredVideoHandoff();
    return handled;
  }

  bool pollVideoWindowInput(InputEvent& event) {
    return videoSessions_.pollWindowInput(event);
  }

  bool handleVideoWindowInputEvent(const InputEvent& event) {
    const bool handled = videoSessions_.handleWindowInputEvent(event);
    drainVideoSessionEvents();
    resumeDeferredVideoHandoff();
    return handled;
  }

  bool handleControlCommand(PlaybackControlCommand command) {
    return applyControlDispatch(
        playbackControl_.dispatch(command, videoSessionRef()));
  }

  bool handleSystemControlCommand(
      const PlaybackControlCommandEvent& event) {
    return applyControlDispatch(
        playbackControl_.dispatch(event, videoSessionRef()));
  }

  bool seekToRatio(double ratio) {
    return playbackControl_.seekToRatio(ratio, videoSessionRef());
  }

  bool toggleWindowPresentation() {
    const bool handled = videoSessions_.toggleWindowPresentation();
    drainVideoSessionEvents();
    return handled;
  }

  bool togglePictureInPicture() {
    const bool handled = videoSessions_.togglePictureInPicture();
    drainVideoSessionEvents();
    return handled;
  }

  bool toggleFullscreen() {
    const bool handled = videoSessions_.toggleFullscreen();
    drainVideoSessionEvents();
    return handled;
  }

  bool activateVideoPresentation() {
    const bool handled = videoSessions_.activatePresentation();
    drainVideoSessionEvents();
    return handled;
  }

  void handleMediaTaskCompletion(
      const media_processing::TaskCompletion& completion) {
    if (videoSessions_.empty()) return;
    if (std::optional<playback_media_processing::Completion> projected =
            media_processing::completionForPlayback(completion)) {
      synchronizeMediaTaskActivity();
      videoSessions_.mediaTaskFinished(*projected);
      resumeDeferredVideoHandoff();
    }
  }

  void synchronizeMediaTaskActivity() {
    if (videoSessions_.empty()) return;
    // Offline processing is application-scoped. Keep its status and direct
    // cancellation available on whichever playback surface is active, even
    // after navigating away from the source media.
    videoSessions_.mediaTaskActivityChanged(
        media_processing::activityForPlayback(
            services_.mediaProcessing.activity()));
  }

  void requestQuit() {
    if (!videoSessions_.requestQuit()) {
      pendingAudioFallback_.reset();
      pendingCommand_.reset();
      releaseForegroundPlayback();
      publishEvent(QuitRequested{});
    }
  }

  bool resolveAudioFallback(tui_media_activation::DecisionId decision,
                            bool playAudio) {
    return resolveAudioFallbackImpl(decision, playAudio);
  }

 private:
  struct PreparedPlayback {
    playback_queue::Queue::PreparedActivation activation;
  };

  using VideoSessions = application_playback::VideoSessionHost;

  struct PendingAudioFallback {
    tui_media_activation::DecisionId decision;
    playback_queue::Queue::PreparedActivation activation;
    std::filesystem::path file;
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
        !externalHandoff_.matches(request.id)) {
      return false;
    }
    std::optional<Command> command = externalHandoff_.accept(request.id);
    if (!command) return false;
    pendingCommand_.emplace(std::move(*command));
    return true;
  }

  void drainVideoSessionEvents() {
    std::vector<playback_session::Event> sessionEvents =
        videoSessions_.drainEvents();
    for (const playback_session::Event& event : sessionEvents) {
      if (const auto* request =
              std::get_if<playback_session_exit::HandoffRequest>(&event)) {
        const bool accepted = acceptSessionHandoff(*request);
        videoSessions_.resolveHandoff(request->id, accepted);
        continue;
      }
      if (const auto* cancellation =
              std::get_if<playback_session_exit::HandoffCancellation>(
                  &event)) {
        (void)externalHandoff_.cancel(cancellation->id);
        continue;
      }
      publishEvent(ActivateBrowserSurface{});
    }
  }

  application_playback::VideoHandoffStart
  tryStartDeferredVideoHandoff() {
    if (videoSessions_.empty()) {
      return application_playback::VideoHandoffStart::NoPendingCommand;
    }
    const application_playback::VideoHandoffStart start =
        externalHandoff_.tryStart(videoSessions_);
    if (start == application_playback::VideoHandoffStart::RequestStarted ||
        start == application_playback::VideoHandoffStart::Failed) {
      drainVideoSessionEvents();
    }
    return start;
  }

  void resumeDeferredVideoHandoff() {
    if (videoSessions_.empty() || !externalHandoff_.awaitingRequest()) return;
    const application_playback::VideoHandoffStart start =
        tryStartDeferredVideoHandoff();
    if (start ==
        application_playback::VideoHandoffStart::WaitingForInteraction) {
      return;
    }
    if (start ==
        application_playback::VideoHandoffStart::RequestStarted) {
      clearCommandError();
      return;
    }

    (void)reject(
        MediaCommandFailureKind::Busy,
        "Could not complete the requested media change because the current "
        "playback session could not hand off control.");
  }

  MediaCommandResult requestVideoHandoff(Command command) {
    if (!videoSessions_.ready()) {
      return reject(MediaCommandFailureKind::Busy, {});
    }
    if (pendingCommand_ || !externalHandoff_.empty()) {
      return reject(MediaCommandFailureKind::Busy,
                    "Another media change is already pending.");
    }
    if (!externalHandoff_.enqueue(std::move(command))) {
      return reject(MediaCommandFailureKind::Busy, {});
    }
    const application_playback::VideoHandoffStart start =
        tryStartDeferredVideoHandoff();
    if (start == application_playback::VideoHandoffStart::Failed ||
        start ==
            application_playback::VideoHandoffStart::NoPendingCommand) {
      return reject(
          MediaCommandFailureKind::Busy,
          "Could not complete the requested media change because the current "
          "playback session could not hand off control.");
    }
    clearCommandError();
    return MediaCommandResult::deferred();
  }

  MediaCommandResult submit(Command command) {
    if (!videoSessions_.empty()) {
      return requestVideoHandoff(std::move(command));
    }
    if (pendingCommand_ || pendingAudioFallback_) {
      return reject(MediaCommandFailureKind::Busy, {});
    }
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
      if (result.isDeferred()) break;
      if (!videoSessions_.empty()) break;
      command = std::exchange(pendingCommand_, std::nullopt);
    }
    clearCommandError();
    return result;
  }

  std::optional<MediaCommandResult> drainPendingCommands() {
    if (!videoSessions_.empty() || pendingAudioFallback_ || driving_ ||
        !pendingCommand_) {
      return std::nullopt;
    }
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

  bool foregroundPlaybackReady() {
    if (!interactivePlayback_) {
      interactivePlayback_.emplace(
          services_.mediaProcessing.acquireInteractivePlayback());
    }
    return interactivePlayback_->ready();
  }

  void releaseForegroundPlayback() { interactivePlayback_.reset(); }

  bool applyControlDispatch(
      application_playback::ControlDispatch dispatch) {
    if (const auto* videoDispatch =
            std::get_if<application_playback::VideoControlDispatched>(
                &dispatch)) {
      drainVideoSessionEvents();
      if (videoDispatch->retireControlSession) {
        playbackControl_.endSession();
      }
      return videoDispatch->handled;
    }
    if (const auto* transportRequest =
            std::get_if<application_playback::QueueTransportRequested>(
                &dispatch)) {
      return transport(transportRequest->direction).accepted();
    }
    return std::holds_alternative<application_playback::ControlApplied>(
        dispatch);
  }

  MediaCommandResult presentPlayback(
      playback_queue::Queue::PreparedActivation activation) {
    const playback_route::Route& route = activation.route();
    const PlaybackTarget target = route.target;
    const std::filesystem::path& targetFile = playbackTargetFile(target);
    const bool videoTarget = isSupportedVideoExt(targetFile);
    if (videoTarget && !foregroundPlaybackReady()) {
      pendingCommand_.emplace(PreparedPlayback{std::move(activation)});
      return MediaCommandResult::deferred();
    }
    if (!videoTarget) releaseForegroundPlayback();

    if (route.videoContinuation) {
      continuationState_ = *route.videoContinuation;
    }
    publishEvent(ApplyAudioPictureInPicture{route.audioPictureInPicture});

    if (const std::optional<int> trackIndex =
            playbackTargetTrackIndex(target)) {
      playbackControl_.endSession();
      if (!audioPlayback().startFile(targetFile, *trackIndex)) {
        publishEvent(AudioPlaybackFailed{targetFile});
        return MediaCommandResult::rejected(
            {MediaCommandFailureKind::PlaybackFailed, {}});
      }
      services_.queue.commit(std::move(activation));
      playbackControl_.beginSession();
      return MediaCommandResult::applied();
    }
    if (isSupportedImageExt(targetFile)) {
      return MediaCommandResult::rejected(
          {MediaCommandFailureKind::Unsupported,
           "The image request did not contain an image sequence."});
    }
    if (!isSupportedVideoExt(targetFile)) {
      playbackControl_.endSession();
      if (!audioPlayback().startFile(targetFile, 0)) {
        publishEvent(AudioPlaybackFailed{targetFile});
        return MediaCommandResult::rejected(
            {MediaCommandFailureKind::PlaybackFailed, {}});
      }
      services_.queue.commit(std::move(activation));
      playbackControl_.beginSession();
      return MediaCommandResult::applied();
    }

    if (!videoSessions_.empty()) {
      return reject(MediaCommandFailureKind::Busy,
                    "Another playback session is already active.");
    }
    playbackControl_.endSession();
    playback_session::VideoSessionRequest sessionRequest(
        services_.mediaProcessingActions);
    sessionRequest.config = sessionConfig(services_.videoConfig,
                                          continuationState_);
    sessionRequest.continuityState = continuationState_;
    sessionRequest.sessionIntent = route.sessionIntent;
    sessionRequest.capabilities.transportHandoff = true;
    sessionRequest.capabilities.openFilesHandoff = true;
    sessionRequest.capabilities.browserSurfaceActivation = true;
    VideoSessions::StartResult start = videoSessions_.start(
        std::move(sessionRequest), std::move(activation));
    if (auto* rejected =
            std::get_if<VideoSessions::StartRejected>(&start)) {
      if (rejected->failure ==
          application_playback::VideoSessionStartFailure::HostOccupied) {
        return reject(MediaCommandFailureKind::Busy,
                      "Another playback session is already active.");
      }
      releaseForegroundPlayback();
      publishEvent(VideoPlaybackFailed{
          targetFile,
          {"Video playback could not be started.",
           "The application video-session factory returned no session."}});
      playbackStateChanged();
      return MediaCommandResult::handledWithoutPlayback();
    }
    if (std::holds_alternative<VideoSessions::OpenPending>(start)) {
      playbackStateChanged();
      return MediaCommandResult::deferred();
    }
    return finishVideoOpen(
        std::move(std::get<VideoSessions::OpenFinished>(start)));
  }

  MediaCommandResult finishVideoOpen(
      VideoSessions::OpenFinished opened) {
    const std::filesystem::path& targetFile =
        playbackTargetFile(opened.activation.route().target);
    if (std::holds_alternative<playback_session::OpenReady>(opened.outcome)) {
      services_.queue.commit(std::move(opened.activation));
      playbackControl_.beginSession();
      playbackStateChanged();
      return MediaCommandResult::applied();
    }
    if (auto* fallback =
            std::get_if<playback_session::OpenAudioFallback>(
                &opened.outcome)) {
      const tui_media_activation::DecisionId decision = nextDecisionId();
      pendingAudioFallback_.emplace(
          PendingAudioFallback{decision, std::move(opened.activation),
                               targetFile});
      publishEvent(tui_media_activation::AudioFallbackRequest{
          decision, targetFile, std::move(fallback->reason)});
      playbackStateChanged();
      return MediaCommandResult::deferred();
    }
    if (std::holds_alternative<playback_session::OpenQuitApplication>(
            opened.outcome)) {
      releaseForegroundPlayback();
      enqueueQuit();
      playbackStateChanged();
      return MediaCommandResult::handledWithoutPlayback();
    }
    if (auto* failure =
            std::get_if<playback_session::OpenFailure>(&opened.outcome)) {
      publishEvent(VideoPlaybackFailed{targetFile,
                                       std::move(failure->problem)});
    } else {
      assert(std::holds_alternative<playback_session::OpenCancelled>(
          opened.outcome));
    }
    releaseForegroundPlayback();
    playbackStateChanged();
    return MediaCommandResult::handledWithoutPlayback();
  }

  void finishVideoSession(VideoSessions::SessionFinished finished) {
    const std::filesystem::path completedFile =
        playbackTargetFile(finished.target);
    PlaybackSessionCompletion completion = std::move(finished.completion);
    continuationState_ = std::move(completion.continuityState);
    if (completion.intent != PlaybackSessionExitIntent::QuitApplication &&
        !pendingCommand_) {
      if (std::optional<Command> command = externalHandoff_.release()) {
        pendingCommand_.emplace(std::move(*command));
      }
    }
    externalHandoff_.clear();
    if (completion.intent == PlaybackSessionExitIntent::QuitApplication) {
      enqueueQuit();
    }
    if (completion.failure) {
      publishEvent(VideoPlaybackFailed{completedFile,
                                       std::move(*completion.failure)});
    }
    if (!pendingCommand_) releaseForegroundPlayback();
    playbackStateChanged();
  }

  MediaCommandResult dispatch(PreparedPlayback playback) {
    return presentPlayback(std::move(playback.activation));
  }

  MediaCommandResult dispatch(tui_media_activation::ShowImages image) {
    releaseForegroundPlayback();
    publishEvent(ShowImages{image.route.audioPictureInPicture,
                            std::move(image.sequence)});
    return MediaCommandResult::applied();
  }

  MediaCommandResult dispatch(
      tui_media_activation::OpenDirectory directory) {
    releaseForegroundPlayback();
    publishEvent(OpenBrowserDirectory{std::move(directory.path)});
    return MediaCommandResult::applied();
  }

  MediaCommandResult dispatch(Quit) {
    releaseForegroundPlayback();
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

  void playbackStateChanged() {
    publishEvent(PlaybackStateChanged{});
  }

  tui_media_activation::DecisionId nextDecisionId() {
    ++lastDecisionValue_;
    if (lastDecisionValue_ == 0) ++lastDecisionValue_;
    return {lastDecisionValue_};
  }

  bool resolveAudioFallbackImpl(tui_media_activation::DecisionId decision,
                                bool playAudio) {
    if (!pendingAudioFallback_ ||
        pendingAudioFallback_->decision != decision) {
      return false;
    }

    PendingAudioFallback pending = std::move(*pendingAudioFallback_);
    pendingAudioFallback_.reset();
    releaseForegroundPlayback();
    if (playAudio) {
      if (!audioPlayback().startFile(pending.file, 0)) {
        publishEvent(AudioPlaybackFailed{pending.file});
      } else {
        services_.queue.commit(std::move(pending.activation));
        playbackControl_.beginSession();
      }
    }
    playbackStateChanged();
    clearCommandError();
    return true;
  }

  void publishEvent(Event event) {
    events_.push_back(std::move(event));
  }

  Services services_;
  application_playback::PlaybackControlRouter playbackControl_;
  VideoSessions videoSessions_;
  PlaybackSessionContinuationState continuationState_;
  std::optional<media_processing::Coordinator::InteractivePlaybackLease>
      interactivePlayback_;
  std::optional<PendingAudioFallback> pendingAudioFallback_;
  std::optional<Command> pendingCommand_;
  application_playback::VideoHandoffController<Command> externalHandoff_;
  std::uint64_t lastDecisionValue_ = 0;
  std::string commandError_;
  // Commands and session pumping are owner-thread operations. Keeping their
  // events as an outbox avoids pretending they are asynchronous wait sources.
  std::vector<Event> events_;
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

bool TuiMediaCoordinator::videoReady() const { return impl_->videoReady(); }

PlaybackControlSessionId TuiMediaCoordinator::controlSessionId() const {
  return impl_->controlSessionId();
}

PlaybackShellTerminalRole TuiMediaCoordinator::terminalRole() const {
  return impl_->terminalRole();
}

std::optional<TuiMediaCoordinator::VideoSnapshot>
TuiMediaCoordinator::videoSnapshot() const {
  return impl_->videoSnapshot();
}

std::optional<playback_session::TransitionSnapshot>
TuiMediaCoordinator::videoTransitionSnapshot() const {
  return impl_->videoTransitionSnapshot();
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

bool TuiMediaCoordinator::canAcceptExternalMediaChange() const {
  return impl_->canAcceptExternalMediaChange();
}

void TuiMediaCoordinator::setExternalInputModal(bool modal) {
  impl_->setExternalInputModal(modal);
}

bool TuiMediaCoordinator::handleVideoInputEvent(const InputEvent& event) {
  return impl_->handleVideoInputEvent(event);
}

bool TuiMediaCoordinator::pollVideoWindowInput(InputEvent& event) {
  return impl_->pollVideoWindowInput(event);
}

bool TuiMediaCoordinator::handleVideoWindowInputEvent(
    const InputEvent& event) {
  return impl_->handleVideoWindowInputEvent(event);
}

bool TuiMediaCoordinator::handleControlCommand(
    PlaybackControlCommand command) {
  return impl_->handleControlCommand(command);
}

bool TuiMediaCoordinator::handleSystemControlCommand(
    const PlaybackControlCommandEvent& event) {
  return impl_->handleSystemControlCommand(event);
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

bool TuiMediaCoordinator::resolveAudioFallback(
    tui_media_activation::DecisionId decision, bool playAudio) {
  return impl_->resolveAudioFallback(decision, playAudio);
}

void TuiMediaCoordinator::requestQuit() { impl_->requestQuit(); }
