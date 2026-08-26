#include "session.h"

#include <atomic>
#include <cassert>
#include <functional>
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
        enableAudio(request.config.enableAudio && audioIsEnabled()),
        host({request.file, dependencies.input, dependencies.screen,
              dependencies.baseStyle, dependencies.accentStyle,
              dependencies.dimStyle, enableAscii}) {}

  ~Impl() { shutdownLoop(); }

  PlaybackSessionBootstrapOutcome bootstrap() {
    PlaybackSessionBootstrap bootstrapper(
        {request.file,
         dependencies.input,
         dependencies.screen,
         dependencies.baseStyle,
         dependencies.accentStyle,
         dependencies.dimStyle,
         dependencies.progressEmptyStyle,
         dependencies.progressFrameStyle,
         dependencies.progressStart,
         dependencies.progressEnd,
         enableAudio,
         enableAscii,
         player});
    return bootstrapper.run();
  }

  void prepareSubtitles() {
    subtitleManager.loadForVideo(request.file);
    hasSubtitles = subtitleManager.selectableTrackCount() > 0;
    enableSubtitlesShared.store(hasSubtitles);
    host.logSubtitleDetection(subtitleManager);
  }

  void createLoop() {
    loop = std::make_unique<PlaybackLoopRunner>(PlaybackLoopRunner::Args{
        dependencies.screen,
        std::move(request.config),
        player,
        subtitleManager,
        host.perfLog(),
        dependencies.baseStyle,
        dependencies.accentStyle,
        dependencies.dimStyle,
        dependencies.progressEmptyStyle,
        dependencies.progressFrameStyle,
        dependencies.progressStart,
        dependencies.progressEnd,
        host.timingSink(),
        host.warningSink(),
        enableSubtitlesShared,
        host.windowTitle(),
        std::move(request.file),
        enableAudio,
        hasSubtitles,
        std::move(request.requestTransportCommand),
        std::move(request.requestOpenFiles),
        std::move(request.mediaProcessingActions),
        std::move(request.activateBrowserSurface),
        std::move(request.continuityState),
        request.sessionIntent});
  }

  void shutdownLoop() {
    if (loop && !loopShutdown) {
      loop->shutdown();
      loopShutdown = true;
    }
  }

  void finalizePlayback() {
    if (!loop) {
      return;
    }

    shutdownLoop();
    if (!loop->hasRenderFailure()) {
      return;
    }

    host.reportVideoError(loop->renderFailureMessage(),
                          loop->renderFailureDetail());
    loop->renderFailureScreen();
  }

  PlaybackSessionCompletion completePlayback() {
    PlaybackSessionCompletion completion{
        loop->quitApplicationRequested()
            ? PlaybackSessionExitIntent::QuitApplication
            : PlaybackSessionExitIntent::Stop,
        loop->continuationState()};
    finalizePlayback();
    lifecycle = Lifecycle::Finished;
    return completion;
  }

  PlaybackSessionOpenOutcome open() {
    assert(lifecycle == Lifecycle::Created);
    if (!host.initialize()) {
      lifecycle = Lifecycle::Finished;
      return PlaybackSessionOpenOutcome::HandledWithoutPlayback;
    }

    const PlaybackSessionBootstrapOutcome bootstrapOutcome = bootstrap();
    if (bootstrapOutcome ==
        PlaybackSessionBootstrapOutcome::QuitApplication) {
      lifecycle = Lifecycle::Finished;
      return PlaybackSessionOpenOutcome::QuitApplicationRequested;
    }
    if (bootstrapOutcome == PlaybackSessionBootstrapOutcome::PlayAudioOnly) {
      lifecycle = Lifecycle::Finished;
      return PlaybackSessionOpenOutcome::AudioFallbackRequested;
    }
    if (bootstrapOutcome == PlaybackSessionBootstrapOutcome::Handled) {
      lifecycle = Lifecycle::Finished;
      return PlaybackSessionOpenOutcome::HandledWithoutPlayback;
    }

    prepareSubtitles();
    createLoop();
    lifecycle = Lifecycle::Ready;
    return PlaybackSessionOpenOutcome::Ready;
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
  std::atomic<bool> enableSubtitlesShared{false};
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

PlaybackSessionOpenOutcome PlaybackSession::open() { return impl_->open(); }

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

int PlaybackSession::nextWakeTimeoutMs() const {
  assert(impl_->canControl());
  return impl_->loop->nextWakeTimeoutMs();
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

bool PlaybackSession::requestHandoff(
    std::function<void(bool)> completion) {
  return impl_->canControl() &&
         impl_->loop->requestHandoff(std::move(completion));
}

void PlaybackSession::subtitleGenerationFinished(
    const std::filesystem::path& preferredSubtitleTrack, bool success,
    std::string status) {
  if (impl_->canControl()) {
    impl_->loop->subtitleGenerationFinished(preferredSubtitleTrack, success,
                                            std::move(status));
  }
}

void PlaybackSession::mediaTaskFinished(std::string status) {
  if (impl_->canControl()) {
    impl_->loop->mediaTaskFinished(std::move(status));
  }
}

void PlaybackSession::requestStop() {
  if (impl_->canControl()) impl_->loop->requestStop();
}

void PlaybackSession::requestQuit() {
  if (impl_->canControl()) impl_->loop->requestQuit();
}
