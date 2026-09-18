#include "session.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <memory>
#include <utility>

#include "audioplayback.h"
#include "core/path_identity.h"
#include "playback/session/bootstrap_input.h"
#include "playback/session/subtitle_loader.h"
#include "playback/video/player.h"
#include "bootstrap.h"
#include "host.h"
#include "loop.h"
#include "playback/video/subtitle/manager.h"

namespace {

double indeterminateActivity(std::chrono::steady_clock::time_point started,
                             std::chrono::steady_clock::time_point now) {
  constexpr double kPulseSeconds = 1.6;
  const double elapsed =
      std::chrono::duration<double>(now - started).count();
  const double phase = std::fmod(elapsed, kPulseSeconds);
  return phase <= kPulseSeconds * 0.5
             ? phase / (kPulseSeconds * 0.5)
             : (kPulseSeconds - phase) / (kPulseSeconds * 0.5);
}

class SessionOpeningBackend final : public playback_session::OpeningBackend {
 public:
  struct SubtitlePublication {
    bool available = false;
    bool preferredTrackSelected = false;
    bool reload = false;
  };

  SessionOpeningBackend(
      Player& player, SubtitleManager& subtitles,
      playback_session::SubtitleLoadService& subtitleLoader)
      : player_(player),
        subtitles_(subtitles),
        subtitleLoader_(subtitleLoader) {}

  std::optional<playback_session::Problem> start(
      const playback_session::OpeningConfiguration& configuration) override {
    closing_ = false;
    PlayerConfig playerConfig;
    playerConfig.file = configuration.file;
    playerConfig.enableAudio = configuration.enableAudio;
    playerConfig.allowDecoderScale = configuration.allowDecoderScale;
    if (!player_.open(playerConfig, nullptr)) {
      return playback_session::Problem{"Failed to open video.", {}};
    }
    beginSubtitleLoad(configuration.file, {}, false);
    return std::nullopt;
  }

  bool initializationDone() override {
    pollSubtitles();
    return player_.initDone();
  }
  bool initializationSucceeded() const override { return player_.initOk(); }
  std::string initializationError() const override {
    return player_.initError();
  }
  bool finishInitialization() override {
    pollSubtitles();
    if (!player_.initDone()) return false;
    (void)publishSubtitlesIfReady();
    return true;
  }
  void requestClose() override {
    closing_ = true;
    requestSubtitleCancellation();
    player_.requestClose();
  }
  bool closeReady() override {
    return player_.closeReady();
  }
  bool finishClose() override {
    if (!closeReady()) return false;
    loadedSubtitles_.reset();
    if (!player_.finishClose()) return false;
    closing_ = false;
    return true;
  }
  std::vector<NativeWaitHandle> waitHandles() const override {
    std::vector<NativeWaitHandle> handles;
    handles.reserve(2);
    const NativeWaitHandle playerHandle =
        closing_ ? player_.closeWaitHandle()
                 : player_.statusChangeWaitHandle();
    if (playerHandle) {
      handles.push_back(playerHandle);
    }
    if (subtitleRequest_) {
      if (NativeWaitHandle subtitleHandle =
              subtitleLoader_.waitHandle(*subtitleRequest_)) {
        handles.push_back(subtitleHandle);
      }
    }
    return handles;
  }

  std::optional<SubtitlePublication> publishSubtitlesIfReady() {
    pollSubtitles();
    if (subtitleRequest_ || subtitlePublished_) return std::nullopt;
    subtitles_ = loadedSubtitles_ ? std::move(*loadedSubtitles_)
                                  : SubtitleManager{};
    loadedSubtitles_.reset();
    const bool available = subtitles_.selectableTrackCount() > 0;
    const bool preferredTrackSelected =
        available && !preferredTrack_.empty() &&
        subtitles_.selectTrackForFile(preferredTrack_);
    subtitlePublished_ = true;
    preferredTrack_.clear();
    return SubtitlePublication{available, preferredTrackSelected,
                               publicationIsReload_};
  }

  void requestSubtitleReload(const std::filesystem::path& file,
                             const std::filesystem::path& preferredTrack) {
    beginSubtitleLoad(file, preferredTrack, true);
  }

  void requestSubtitleCancellation() {
    retireSubtitleRequest();
    loadedSubtitles_.reset();
    preferredTrack_.clear();
    publicationIsReload_ = false;
    subtitlePublished_ = true;
  }

  NativeWaitHandle subtitleWaitHandle() const {
    return subtitleRequest_ ? subtitleLoader_.waitHandle(*subtitleRequest_)
                            : NativeWaitHandle{};
  }

 private:
  void retireSubtitleRequest() {
    if (!subtitleRequest_) return;
    (void)subtitleLoader_.cancel(*subtitleRequest_);
    subtitleRequest_.reset();
  }

  void beginSubtitleLoad(const std::filesystem::path& file,
                         const std::filesystem::path& preferredTrack,
                         bool reload) {
    retireSubtitleRequest();
    loadedSubtitles_.reset();
    preferredTrack_ = preferredTrack;
    publicationIsReload_ = reload;
    subtitlePublished_ = false;
    subtitleRequest_ = subtitleLoader_.start(file);
  }

  void pollSubtitles() {
    if (!subtitleRequest_) return;
    if (!subtitleLoader_.active(*subtitleRequest_)) {
      subtitleRequest_.reset();
      return;
    }
    std::optional<playback_session::SubtitleLoadService::Completion>
        completion = subtitleLoader_.poll(*subtitleRequest_);
    if (!completion) return;
    if (completion->requestId != *subtitleRequest_) return;
    subtitleRequest_.reset();
    if (completion->subtitles) {
      loadedSubtitles_.emplace(std::move(*completion->subtitles));
    }
  }

  Player& player_;
  SubtitleManager& subtitles_;
  playback_session::SubtitleLoadService& subtitleLoader_;
  std::optional<SubtitleManager> loadedSubtitles_;
  std::optional<playback_session::SubtitleLoadService::RequestId>
      subtitleRequest_;
  std::filesystem::path preferredTrack_;
  bool publicationIsReload_ = false;
  bool subtitlePublished_ = false;
  bool closing_ = false;
};

}  // namespace

struct PlaybackSession::Impl {
  enum class Lifecycle {
    Created,
    Opening,
    Ready,
    Running,
    Closing,
    Finished,
  };

  Impl(playback_session::VideoSessionRequest startRequest,
       Dependencies sessionDependencies)
      : request(std::move(startRequest)),
        dependencies(std::move(sessionDependencies)),
        enableAscii(request.config.enableAscii),
        enableAudio(request.config.enableAudio &&
                    dependencies.audioPlayback.enabled()),
        host({request.file, dependencies.screen, dependencies.gpu,
              enableAscii}),
        player(dependencies.audioPlayback, dependencies.gpu),
        openingBackend(player, subtitleManager,
                       dependencies.subtitleLoader) {}

  ~Impl() { shutdownLoop(); }

  void createBootstrap() {
    bootstrapper.emplace(PlaybackSessionBootstrap::Args{
        request.file, enableAudio, enableAscii, openingBackend});
  }

  void publishSubtitleState() {
    hasSubtitles = subtitleManager.selectableTrackCount() > 0;
    host.logSubtitleDetection(subtitleManager);
  }

  void publishCompletedSubtitleLoad() {
    std::optional<SessionOpeningBackend::SubtitlePublication> publication =
        openingBackend.publishSubtitlesIfReady();
    if (!publication) return;
    publishSubtitleState();
    if (loop && (lifecycle == Lifecycle::Ready ||
                 lifecycle == Lifecycle::Running)) {
      loop->subtitlesLoaded(publication->available, publication->reload,
                            publication->preferredTrackSelected);
    }
  }

  void createLoop() {
    loop = std::make_unique<PlaybackLoopRunner>(PlaybackLoopRunner::Args{
        dependencies.screen,
        dependencies.audioPlayback,
        dependencies.gpu,
        std::move(request.config),
        player,
        subtitleManager,
        host.perfLog(),
        dependencies.appearance.baseStyle,
        dependencies.appearance.accentStyle,
        dependencies.appearance.dimStyle,
        dependencies.appearance.progressEmptyStyle,
        dependencies.appearance.progressFrameStyle,
        dependencies.appearance.progressStart,
        dependencies.appearance.progressEnd,
        host.timingSink(),
        host.warningSink(),
        hasSubtitles,
        host.windowTitle(),
        request.file,
        enableAudio,
        hasSubtitles,
        !openingBackend.subtitleWaitHandle(),
        request.capabilities,
        std::move(request.mediaProcessingActions),
        std::move(request.continuityState),
        request.sessionIntent});
  }

  void shutdownLoop() {
    if (lifecycle == Lifecycle::Opening && bootstrapper) {
      // Exceptional owner teardown cannot keep pumping the opening state
      // machine, but it must still retire the borrowed subtitle request and
      // tell the player worker to stop before member destruction joins it.
      bootstrapper->requestCancel();
    }
    if (loop && !loopShutdown) {
      loop->shutdown();
      loopShutdown = true;
    }
  }

  std::optional<playback_session::Problem> playbackFailure() {
    if (!loop) {
      return std::nullopt;
    }
    if (!loop->hasRenderFailure()) {
      return std::nullopt;
    }

    return host.recordVideoError(loop->renderFailureMessage(),
                                 loop->renderFailureDetail());
  }

  void beginClosing() {
    assert(loop);
    closingIntent =
        loop->quitApplicationRequested()
            ? PlaybackSessionExitIntent::QuitApplication
            : PlaybackSessionExitIntent::Stop;
    closingContinuityState = loop->continuationState();
    openingBackend.requestSubtitleCancellation();
    loop->beginShutdown();
    closingStarted = std::chrono::steady_clock::now();
    closingNextDraw = closingStarted;
    lifecycle = Lifecycle::Closing;
  }

  std::optional<PlaybackSessionCompletion> finishClosing() {
    assert(lifecycle == Lifecycle::Closing && loop && closingIntent);
    const auto now = std::chrono::steady_clock::now();
    if (now >= closingNextDraw) {
      closingNextDraw = now + kTransitionRedrawInterval;
    }
    if (!loop->shutdownReady() || !loop->finishShutdown()) {
      return std::nullopt;
    }
    loopShutdown = true;
    std::optional<playback_session::Problem> failure = playbackFailure();
    PlaybackSessionCompletion completion{
        *closingIntent, std::move(closingContinuityState), std::move(failure)};
    closingIntent.reset();
    lifecycle = Lifecycle::Finished;
    return completion;
  }

  playback_session::OpenOutcome finishOpen(
      playback_session::OpenOutcome outcome) {
    if (!std::holds_alternative<playback_session::OpenReady>(outcome)) {
      bootstrapper.reset();
      lifecycle = Lifecycle::Finished;
      return outcome;
    }

    bootstrapper.reset();
    publishSubtitleState();
    createLoop();
    lifecycle = Lifecycle::Ready;
    return playback_session::OpenReady{};
  }

  std::optional<playback_session::OpenOutcome> startOpen() {
    assert(lifecycle == Lifecycle::Created);
    if (std::optional<playback_session::Problem> failure =
            host.tryInitialize()) {
      lifecycle = Lifecycle::Finished;
      return playback_session::OpenFailure{std::move(*failure)};
    }

    createBootstrap();
    std::optional<playback_session::OpenOutcome> outcome =
        bootstrapper->start();
    if (!outcome) {
      lifecycle = Lifecycle::Opening;
      return std::nullopt;
    }
    return finishOpen(std::move(*outcome));
  }

  std::optional<playback_session::OpenOutcome> pumpOpen() {
    assert(lifecycle == Lifecycle::Opening && bootstrapper);
    std::optional<playback_session::OpenOutcome> outcome =
        bootstrapper->pump();
    if (!outcome) return std::nullopt;
    return finishOpen(std::move(*outcome));
  }

  std::optional<PlaybackSessionCompletion> pump() {
    if (lifecycle == Lifecycle::Closing) return finishClosing();
    assert(lifecycle == Lifecycle::Ready ||
           lifecycle == Lifecycle::Running);
    lifecycle = Lifecycle::Running;
    publishCompletedSubtitleLoad();
    if (loop->pump()) {
      return std::nullopt;
    }
    beginClosing();
    return finishClosing();
  }

  bool canControl() const {
    return loop && (lifecycle == Lifecycle::Ready ||
                    lifecycle == Lifecycle::Running);
  }

  bool opening() const { return lifecycle == Lifecycle::Opening; }

  bool closing() const { return lifecycle == Lifecycle::Closing; }

  static constexpr auto kTransitionRedrawInterval =
      std::chrono::milliseconds(120);

  playback_session::VideoSessionRequest request;
  Dependencies dependencies;
  const bool enableAscii;
  const bool enableAudio;
  PlaybackSessionHost host;
  Player player;
  SubtitleManager subtitleManager;
  SessionOpeningBackend openingBackend;
  std::optional<PlaybackSessionBootstrap> bootstrapper;
  bool hasSubtitles = false;
  bool loopShutdown = false;
  std::optional<PlaybackSessionExitIntent> closingIntent;
  PlaybackSessionContinuationState closingContinuityState;
  std::chrono::steady_clock::time_point closingStarted{};
  std::chrono::steady_clock::time_point closingNextDraw{};
  Lifecycle lifecycle = Lifecycle::Created;
  std::unique_ptr<PlaybackLoopRunner> loop;
};

PlaybackSession::PlaybackSession(playback_session::VideoSessionRequest request,
                                 Dependencies dependencies)
    : impl_(std::make_unique<Impl>(std::move(request),
                                  std::move(dependencies))) {}

PlaybackSession::~PlaybackSession() = default;

PlaybackSession::PlaybackSession(PlaybackSession&&) noexcept = default;

PlaybackSession& PlaybackSession::operator=(PlaybackSession&&) noexcept =
    default;

std::optional<playback_session::OpenOutcome> PlaybackSession::startOpen() {
  return impl_->startOpen();
}

std::optional<playback_session::OpenOutcome> PlaybackSession::pumpOpen() {
  return impl_->pumpOpen();
}

bool PlaybackSession::opening() const { return impl_->opening(); }

bool PlaybackSession::ready() const { return impl_->canControl(); }

std::optional<playback_session::TransitionSnapshot>
PlaybackSession::transitionSnapshot() const {
  if (impl_->opening()) return impl_->bootstrapper->snapshot();
  if (impl_->closing()) {
    return playback_session::TransitionSnapshot{
        impl_->request.file, playback_session::TransitionStage::Closing,
        indeterminateActivity(impl_->closingStarted,
                              std::chrono::steady_clock::now())};
  }
  return std::nullopt;
}

std::optional<PlaybackSessionCompletion> PlaybackSession::pump() {
  return impl_->pump();
}

PlaybackShellTerminalRole PlaybackSession::terminalRole() const {
  if (impl_->opening()) return PlaybackShellTerminalRole::Playback;
  if (impl_->closing()) return PlaybackShellTerminalRole::Playback;
  assert(impl_->canControl());
  return impl_->loop->terminalRole();
}

std::vector<NativeWaitHandle> PlaybackSession::activityWaitHandles() const {
  if (impl_->opening()) {
    return impl_->bootstrapper->waitHandles();
  }
  if (impl_->closing()) {
    return impl_->loop->shutdownWaitHandles();
  }
  assert(impl_->canControl());
  std::vector<NativeWaitHandle> handles =
      impl_->loop->activityWaitHandles();
  if (NativeWaitHandle subtitleHandle =
          impl_->openingBackend.subtitleWaitHandle()) {
    handles.push_back(subtitleHandle);
  }
  return handles;
}

wake_schedule::Deadline PlaybackSession::nextWakeDeadline() const {
  if (impl_->opening()) return impl_->bootstrapper->nextWakeDeadline();
  if (impl_->closing()) return impl_->closingNextDraw;
  assert(impl_->canControl());
  return impl_->loop->nextWakeDeadline();
}

std::optional<playback_session::ViewSnapshot>
PlaybackSession::viewSnapshot() const {
  if (!impl_->canControl()) return std::nullopt;
  return impl_->loop->viewSnapshot();
}

bool PlaybackSession::capturesBrowserInput() const {
  return impl_->opening() || impl_->closing() ||
         (impl_->canControl() && impl_->loop->capturesBrowserInput());
}

void PlaybackSession::setExternalInputModal(bool modal) {
  if (impl_->canControl()) impl_->loop->setExternalInputModal(modal);
}

bool PlaybackSession::handleInputEvent(const InputEvent& event) {
  if (impl_->opening()) {
    return impl_->bootstrapper->handleInputEvent(event);
  }
  if (impl_->closing()) {
    const auto action = playback_session_bootstrap_input::resolve(event);
    if (action ==
        playback_session_bootstrap_input::Action::QuitApplication) {
      impl_->closingIntent = PlaybackSessionExitIntent::QuitApplication;
    }
    return action.has_value() || event.type == InputEvent::Type::Resize;
  }
  return impl_->canControl() && impl_->loop->handleInputEvent(event);
}

bool PlaybackSession::pollWindowInput(InputEvent& event) {
  return impl_->canControl() && impl_->loop->pollWindowInput(event);
}

bool PlaybackSession::handleWindowInputEvent(const InputEvent& event) {
  return impl_->canControl() && impl_->loop->handleWindowInputEvent(event);
}

bool PlaybackSession::handleControlCommand(PlaybackControlCommand command) {
  if (impl_->opening() && command == PlaybackControlCommand::Stop) {
    impl_->bootstrapper->requestCancel();
    return true;
  }
  if (impl_->closing() && command == PlaybackControlCommand::Stop) return true;
  return impl_->canControl() && impl_->loop->handleControlCommand(command);
}

bool PlaybackSession::seekToRatio(double ratio) {
  return impl_->canControl() && impl_->loop->seekToRatio(ratio);
}

bool PlaybackSession::toggleWindowPresentation() {
  return impl_->canControl() && impl_->loop->toggleWindowPresentation();
}

bool PlaybackSession::togglePictureInPicture() {
  return impl_->canControl() && impl_->loop->togglePictureInPicture();
}

bool PlaybackSession::toggleFullscreen() {
  return impl_->canControl() && impl_->loop->toggleFullscreen();
}

bool PlaybackSession::activatePresentation() {
  return impl_->canControl() && impl_->loop->activatePresentation();
}

std::optional<playback_session_exit::RequestId>
PlaybackSession::requestHandoff() {
  if (!impl_->canControl()) return std::nullopt;
  return impl_->loop->requestHandoff();
}

bool PlaybackSession::resolveHandoff(
    playback_session_exit::RequestId requestId, bool accepted) {
  return impl_->canControl() &&
         impl_->loop->resolveHandoff(requestId, accepted);
}

bool PlaybackSession::abortHandoff(
    playback_session_exit::RequestId requestId) {
  return impl_->canControl() && impl_->loop->abortHandoff(requestId);
}

std::vector<playback_session::Event> PlaybackSession::drainEvents() {
  if (!impl_->canControl()) return {};
  return impl_->loop->drainEvents();
}

void PlaybackSession::mediaTaskFinished(
    const playback_media_processing::Completion& completion) {
  if (impl_->canControl()) {
    if (completion.operation ==
            playback_media_processing::Operation::SubtitleGeneration &&
        completion.succeeded() &&
        samePath(completion.sourceFile, impl_->request.file)) {
      impl_->openingBackend.requestSubtitleReload(impl_->request.file,
                                                  completion.outputFile);
    }
    impl_->loop->mediaTaskFinished(completion);
    impl_->publishCompletedSubtitleLoad();
  }
}

void PlaybackSession::mediaTaskActivityChanged(
    std::optional<playback_media_processing::Activity> activity) {
  if (impl_->canControl()) {
    impl_->loop->mediaTaskActivityChanged(std::move(activity));
  }
}

void PlaybackSession::requestStop() {
  if (impl_->opening()) {
    impl_->bootstrapper->requestCancel();
  } else if (impl_->canControl()) {
    impl_->loop->requestStop();
  }
}

void PlaybackSession::requestQuit() {
  if (impl_->opening()) {
    impl_->bootstrapper->requestQuit();
  } else if (impl_->closing()) {
    impl_->closingIntent = PlaybackSessionExitIntent::QuitApplication;
  } else if (impl_->canControl()) {
    impl_->loop->requestQuit();
  }
}
