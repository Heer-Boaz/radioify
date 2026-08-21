#include "session.h"

#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <utility>

#include "audioplayback.h"
#include "playback/video/playback.h"
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

  explicit Impl(Args args)
      : file(std::move(args.file)),
        input(args.input),
        screen(args.screen),
        baseStyle(args.baseStyle),
        accentStyle(args.accentStyle),
        dimStyle(args.dimStyle),
        progressEmptyStyle(args.progressEmptyStyle),
        progressFrameStyle(args.progressFrameStyle),
        progressStart(args.progressStart),
        progressEnd(args.progressEnd),
        config(std::move(args.config)),
        requestTransportCommand(std::move(args.requestTransportCommand)),
        requestOpenFiles(std::move(args.requestOpenFiles)),
        continuityState(args.continuityState),
        sessionIntent(args.sessionIntent),
        enableAscii(config.enableAscii),
        enableAudio(config.enableAudio && audioIsEnabled()),
        host({file, input, screen, baseStyle, accentStyle, dimStyle,
              enableAscii, args.quitAppRequested}) {}

  ~Impl() { shutdownLoop(); }

  PlaybackSessionBootstrapOutcome bootstrap() {
    PlaybackSessionBootstrap bootstrapper(
        {file,       input,
         screen,     baseStyle,
         accentStyle, dimStyle,
         progressEmptyStyle, progressFrameStyle,
         progressStart,      progressEnd,
         enableAudio, enableAscii,
         player,     host.quitApplicationRequestedPtr()});
    return bootstrapper.run();
  }

  void prepareSubtitles() {
    subtitleManager.loadForVideo(file);
    hasSubtitles = subtitleManager.selectableTrackCount() > 0;
    enableSubtitlesShared.store(hasSubtitles);
    host.logSubtitleDetection(subtitleManager);
  }

  void createLoop() {
    loop = std::make_unique<PlaybackLoopRunner>(PlaybackLoopRunner::Args{
        screen,
        config,
        player,
        subtitleManager,
        host.perfLog(),
        baseStyle,
        accentStyle,
        dimStyle,
        progressEmptyStyle,
        progressFrameStyle,
        progressStart,
        progressEnd,
        host.timingSink(),
        host.warningSink(),
        enableSubtitlesShared,
        host.windowTitle(),
        file,
        enableAscii,
        enableAudio,
        hasSubtitles,
        host.quitApplicationRequestedPtr(),
        requestTransportCommand,
        requestOpenFiles,
        continuityState,
        sessionIntent});
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

  void completePlayback() {
    if (continuityState) {
      *continuityState = loop->continuationState();
    }
    finalizePlayback();
    lifecycle = Lifecycle::Finished;
  }

  PlaybackSessionOpenOutcome open() {
    assert(lifecycle == Lifecycle::Created);
    if (!host.initialize()) {
      lifecycle = Lifecycle::Finished;
      return PlaybackSessionOpenOutcome::HandledWithoutPlayback;
    }

    const PlaybackSessionBootstrapOutcome bootstrapOutcome = bootstrap();
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

  bool pump() {
    assert(lifecycle == Lifecycle::Ready ||
           lifecycle == Lifecycle::Running);
    lifecycle = Lifecycle::Running;
    if (loop->pump()) {
      return true;
    }
    completePlayback();
    return false;
  }

  bool canControl() const {
    return loop && (lifecycle == Lifecycle::Ready ||
                    lifecycle == Lifecycle::Running);
  }

  std::filesystem::path file;
  ConsoleInput& input;
  ConsoleScreen& screen;
  const Style& baseStyle;
  const Style& accentStyle;
  const Style& dimStyle;
  const Style& progressEmptyStyle;
  const Style& progressFrameStyle;
  const Color& progressStart;
  const Color& progressEnd;
  VideoPlaybackConfig config;
  std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
  std::function<bool(const std::vector<std::filesystem::path>&)> requestOpenFiles;
  PlaybackSessionContinuationState* continuityState = nullptr;
  PlaybackSessionIntent sessionIntent = PlaybackSessionIntent::View;
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

PlaybackSession::PlaybackSession(Args args)
    : impl_(std::make_unique<Impl>(std::move(args))) {}

PlaybackSession::~PlaybackSession() = default;

PlaybackSession::PlaybackSession(PlaybackSession&&) noexcept = default;

PlaybackSession& PlaybackSession::operator=(PlaybackSession&&) noexcept =
    default;

PlaybackSessionOpenOutcome PlaybackSession::open() { return impl_->open(); }

bool PlaybackSession::pump() {
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

void PlaybackSession::requestStop() {
  if (impl_->canControl()) impl_->loop->requestStop();
}

void PlaybackSession::requestQuit() {
  if (impl_->canControl()) impl_->loop->requestQuit();
}
