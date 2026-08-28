#include "session.h"

#include <cassert>
#include <memory>
#include <utility>

#include "audioplayback.h"
#include "playback/video/player.h"
#include "bootstrap.h"
#include "host.h"
#include "loop.h"
#include "playback/video/subtitle/manager.h"

struct PlaybackSession::Impl {
  enum class Lifecycle {
    Created,
    Ready,
    Running,
    Finished,
  };

  Impl(Request startRequest, Dependencies sessionDependencies)
      : request(std::move(startRequest)),
        dependencies(std::move(sessionDependencies)),
        enableAscii(request.config.enableAscii),
        enableAudio(request.config.enableAudio &&
                    dependencies.audioPlayback.enabled()),
        host({request.file, dependencies.screen, dependencies.gpu,
              enableAscii}),
        player(dependencies.audioPlayback, dependencies.gpu) {}

  ~Impl() { shutdownLoop(); }

  playback_session::OpenOutcome bootstrap() {
    PlaybackSessionBootstrap bootstrapper(
        {request.file,
         dependencies.input,
         dependencies.screen,
         dependencies.appearance.baseStyle,
         dependencies.appearance.accentStyle,
         dependencies.appearance.dimStyle,
         dependencies.appearance.progressEmptyStyle,
         dependencies.appearance.progressFrameStyle,
         dependencies.appearance.progressStart,
         dependencies.appearance.progressEnd,
         enableAudio,
         enableAscii,
         player});
    return bootstrapper.run();
  }

  void prepareSubtitles() {
    subtitleManager.loadForVideo(request.file);
    hasSubtitles = subtitleManager.selectableTrackCount() > 0;
    host.logSubtitleDetection(subtitleManager);
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
        std::move(request.file),
        enableAudio,
        hasSubtitles,
        request.capabilities,
        std::move(request.mediaProcessingActions),
        std::move(request.continuityState),
        request.sessionIntent});
  }

  void shutdownLoop() {
    if (loop && !loopShutdown) {
      loop->shutdown();
      loopShutdown = true;
    }
  }

  std::optional<playback_session::Problem> finalizePlayback() {
    if (!loop) {
      return std::nullopt;
    }

    shutdownLoop();
    if (!loop->hasRenderFailure()) {
      return std::nullopt;
    }

    return host.recordVideoError(loop->renderFailureMessage(),
                                 loop->renderFailureDetail());
  }

  PlaybackSessionCompletion completePlayback() {
    const PlaybackSessionExitIntent intent =
        loop->quitApplicationRequested()
            ? PlaybackSessionExitIntent::QuitApplication
            : PlaybackSessionExitIntent::Stop;
    PlaybackSessionContinuationState continuityState =
        loop->continuationState();
    std::optional<playback_session::Problem> failure = finalizePlayback();
    PlaybackSessionCompletion completion{intent, std::move(continuityState),
                                         std::move(failure)};
    lifecycle = Lifecycle::Finished;
    return completion;
  }

  playback_session::OpenOutcome open() {
    assert(lifecycle == Lifecycle::Created);
    if (std::optional<playback_session::Problem> failure =
            host.tryInitialize()) {
      lifecycle = Lifecycle::Finished;
      return playback_session::OpenFailure{std::move(*failure)};
    }

    playback_session::OpenOutcome bootstrapOutcome = bootstrap();
    if (std::holds_alternative<playback_session::OpenQuitApplication>(
            bootstrapOutcome)) {
      lifecycle = Lifecycle::Finished;
      return bootstrapOutcome;
    }
    if (std::holds_alternative<playback_session::OpenAudioFallback>(
            bootstrapOutcome)) {
      lifecycle = Lifecycle::Finished;
      return bootstrapOutcome;
    }
    if (!std::holds_alternative<playback_session::OpenReady>(
            bootstrapOutcome)) {
      lifecycle = Lifecycle::Finished;
      return bootstrapOutcome;
    }

    prepareSubtitles();
    createLoop();
    lifecycle = Lifecycle::Ready;
    return playback_session::OpenReady{};
  }

  std::optional<PlaybackSessionCompletion> pump() {
    assert(lifecycle == Lifecycle::Ready ||
           lifecycle == Lifecycle::Running);
    lifecycle = Lifecycle::Running;
    if (loop->pump()) {
      return std::nullopt;
    }
    return completePlayback();
  }

  bool canControl() const {
    return loop && (lifecycle == Lifecycle::Ready ||
                    lifecycle == Lifecycle::Running);
  }

  Request request;
  Dependencies dependencies;
  const bool enableAscii;
  const bool enableAudio;
  PlaybackSessionHost host;
  Player player;
  SubtitleManager subtitleManager;
  bool hasSubtitles = false;
  bool loopShutdown = false;
  Lifecycle lifecycle = Lifecycle::Created;
  std::unique_ptr<PlaybackLoopRunner> loop;
};

PlaybackSession::PlaybackSession(Request request, Dependencies dependencies)
    : impl_(std::make_unique<Impl>(std::move(request),
                                  std::move(dependencies))) {}

PlaybackSession::~PlaybackSession() = default;

PlaybackSession::PlaybackSession(PlaybackSession&&) noexcept = default;

PlaybackSession& PlaybackSession::operator=(PlaybackSession&&) noexcept =
    default;

playback_session::OpenOutcome PlaybackSession::open() {
  return impl_->open();
}

std::optional<PlaybackSessionCompletion> PlaybackSession::pump() {
  return impl_->pump();
}

PlaybackShellTerminalRole PlaybackSession::terminalRole() const {
  assert(impl_->canControl());
  return impl_->loop->terminalRole();
}

std::vector<NativeWaitHandle> PlaybackSession::activityWaitHandles() const {
  assert(impl_->canControl());
  return impl_->loop->activityWaitHandles();
}

wake_schedule::Deadline PlaybackSession::nextWakeDeadline() const {
  assert(impl_->canControl());
  return impl_->loop->nextWakeDeadline();
}

PlaybackControlState PlaybackSession::controlState() const {
  assert(impl_->canControl());
  return impl_->loop->controlState();
}

PlaybackPresentationState PlaybackSession::presentationState() const {
  assert(impl_->canControl());
  return impl_->loop->presentationState();
}

bool PlaybackSession::capturesBrowserInput() const {
  return impl_->canControl() && impl_->loop->capturesBrowserInput();
}

void PlaybackSession::setExternalInputModal(bool modal) {
  if (impl_->canControl()) impl_->loop->setExternalInputModal(modal);
}

bool PlaybackSession::handleInputEvent(const InputEvent& event) {
  return impl_->canControl() && impl_->loop->handleInputEvent(event);
}

bool PlaybackSession::handleControlCommand(PlaybackControlCommand command) {
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

std::vector<playback_session::Event> PlaybackSession::drainEvents() {
  if (!impl_->canControl()) return {};
  return impl_->loop->drainEvents();
}

void PlaybackSession::mediaTaskFinished(
    const playback_media_processing::Completion& completion) {
  if (impl_->canControl()) {
    impl_->loop->mediaTaskFinished(completion);
  }
}

void PlaybackSession::requestStop() {
  if (impl_->canControl()) impl_->loop->requestStop();
}

void PlaybackSession::requestQuit() {
  if (impl_->canControl()) impl_->loop->requestQuit();
}
