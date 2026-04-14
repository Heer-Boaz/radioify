#include "playback_session.h"

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

#include "audioplayback.h"
#include "player.h"
#include "playback_session_bootstrap.h"
#include "playback_session_host.h"
#include "playback_session_loop.h"
#include "subtitle_manager.h"

struct PlaybackSession::Impl {
  explicit Impl(Args args)
      : file(args.file),
        input(args.input),
        screen(args.screen),
        baseStyle(args.baseStyle),
        accentStyle(args.accentStyle),
        dimStyle(args.dimStyle),
        progressEmptyStyle(args.progressEmptyStyle),
        progressFrameStyle(args.progressFrameStyle),
        progressStart(args.progressStart),
        progressEnd(args.progressEnd),
        config(args.config),
        requestTransportCommand(std::move(args.requestTransportCommand)),
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
        input,
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
        enableAscii,
        enableAudio,
        hasSubtitles,
        host.quitApplicationRequestedPtr(),
        requestTransportCommand});
  }

  void shutdownLoop() {
    if (loop && !loopShutdown) {
      loop->shutdown();
      loopShutdown = true;
    }
  }

  bool finalizeRun() {
    if (!loop) {
      return true;
    }

    shutdownLoop();
    if (!loop->hasRenderFailure()) {
      return true;
    }

    const bool ok = host.reportVideoError(loop->renderFailureMessage(),
                                          loop->renderFailureDetail());
    loop->renderFailureScreen();
    return ok;
  }

  bool run() {
    if (!host.initialize()) {
      return true;
    }

    const PlaybackSessionBootstrapOutcome bootstrapOutcome = bootstrap();
    if (bootstrapOutcome != PlaybackSessionBootstrapOutcome::ContinueVideo) {
      return bootstrapOutcome ==
             PlaybackSessionBootstrapOutcome::PlayAudioOnly
                 ? false
                 : true;
    }

    prepareSubtitles();
    createLoop();
    loop->run();
    return finalizeRun();
  }

  const std::filesystem::path& file;
  ConsoleInput& input;
  ConsoleScreen& screen;
  const Style& baseStyle;
  const Style& accentStyle;
  const Style& dimStyle;
  const Style& progressEmptyStyle;
  const Style& progressFrameStyle;
  const Color& progressStart;
  const Color& progressEnd;
  const VideoPlaybackConfig& config;
  std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
  const bool enableAscii;
  const bool enableAudio;
  PlaybackSessionHost host;
  Player player;
  SubtitleManager subtitleManager;
  std::atomic<bool> enableSubtitlesShared{false};
  bool hasSubtitles = false;
  bool loopShutdown = false;
  std::unique_ptr<PlaybackLoopRunner> loop;
};

PlaybackSession::PlaybackSession(Args args)
    : impl_(std::make_unique<Impl>(std::move(args))) {}

PlaybackSession::~PlaybackSession() = default;

PlaybackSession::PlaybackSession(PlaybackSession&&) noexcept = default;

PlaybackSession& PlaybackSession::operator=(PlaybackSession&&) noexcept =
    default;

bool PlaybackSession::run() { return impl_->run(); }
