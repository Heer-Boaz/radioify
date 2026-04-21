#include "playback_session_loop.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "asciiart.h"
#include "asciiart_gpu.h"
#include "audioplayback.h"
#include "gpu_shared.h"
#include "player.h"
#include "playback/ascii/playback_frame_output.h"
#include "playback/ascii/playback_screen_renderer.h"
#include "playback_framebuffer_presenter.h"
#include "playback_mode.h"
#include "playback/system_media_transport_controls.h"
#include "playback_session_core.h"
#include "playback_session_input.h"
#include "playback_session_output.h"
#include "playback_session_presentation.h"
#include "playback_session_state.h"
#include "subtitle_manager.h"

namespace {

enum class PlaybackLoopState : uint8_t {
  Running,
  Stopped,
};

bool shouldRenderPlaybackFrame(bool redraw, bool presented,
                               bool overlayRefreshDue,
                               bool debugRefreshDue,
                               PlaybackSessionState playbackState) {
  return redraw || presented ||
         ((overlayRefreshDue || debugRefreshDue) &&
                    playbackState != PlaybackSessionState::Ended);
}

PlaybackLayout initialPlaybackLayout(
    const VideoPlaybackConfig& config,
    const PlaybackSessionContinuationState* continuityState) {
  if (continuityState && continuityState->hasLayout) {
    return continuityState->layout;
  }
  return config.enableWindow ? PlaybackLayout::Window
                             : PlaybackLayout::Terminal;
}

}  // namespace

struct PlaybackLoopRunner::Impl {
  static constexpr auto kSeekThrottleInterval = std::chrono::milliseconds(50);

  ConsoleInput& input;
  ConsoleScreen& screen;
  const VideoPlaybackConfig& config;
  SubtitleManager& subtitleManager;
  PerfLog& perfLog;
  const Style& baseStyle;
  const Style& accentStyle;
  const Style& dimStyle;
  const Style& progressEmptyStyle;
  const Style& progressFrameStyle;
  const Color& progressStart;
  const Color& progressEnd;
  playback_frame_output::LogLineWriter timingSink;
  playback_frame_output::LogLineWriter warningSink;
  std::atomic<bool>& enableSubtitlesShared;
  const std::string& windowTitle;
  const std::filesystem::path& file;
  bool* quitApplicationRequested = nullptr;
  PlaybackSystemControls* systemControls = nullptr;
  std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
  PlaybackSessionContinuationState* continuityState = nullptr;
  PlaybackSessionContinuationState capturedContinuationState;
  const bool enableAscii;
  const bool enableAudio;
  const bool hasSubtitles;

  PlaybackOutputController output;
  PlaybackSessionCore core;
  GpuAsciiRenderer& gpuRenderer;
  AsciiArt art;
  ConsoleScreen pictureInPictureTextScreen;
  std::vector<ScreenCell> pictureInPictureTextCells;
  AsciiArt pictureInPictureTextArt;
  VideoFrame pictureInPictureTextFrame;
  GpuVideoFrameCache pictureInPictureTextFrameCache;
  playback_frame_output::FrameOutputState pictureInPictureTextOutputState;
  bool pendingPictureInPictureOpen = false;
  bool pendingPictureInPictureTextMode = false;
  bool pictureInPictureStartedFromTerminal = false;
  bool redraw = true;
  bool forceRefreshArt = false;
  playback_frame_output::FrameOutputState frameOutputState;
  std::atomic<int64_t> overlayUntilMs{0};
  std::atomic<int> overlayControlHover{-1};
  bool loopStopRequested = false;
  std::chrono::steady_clock::time_point lastOverlayRefresh =
      std::chrono::steady_clock::time_point::min();
  std::chrono::steady_clock::time_point lastDebugRefresh =
      std::chrono::steady_clock::time_point::min();
  std::chrono::steady_clock::time_point lastUiHeartbeat =
      std::chrono::steady_clock::now();

  playback_session_input::PlaybackInputView inputView;
  playback_session_input::PlaybackInputSignals inputSignals;
  playback_session_input::PlaybackSeekState seekState;
  playback_screen_renderer::PlaybackScreenRenderInputs renderInputs;

  explicit Impl(PlaybackLoopRunner::Args args)
      : input(args.input),
        screen(args.screen),
        config(args.config),
        subtitleManager(args.subtitleManager),
        perfLog(args.perfLog),
        baseStyle(args.baseStyle),
        accentStyle(args.accentStyle),
        dimStyle(args.dimStyle),
        progressEmptyStyle(args.progressEmptyStyle),
        progressFrameStyle(args.progressFrameStyle),
        progressStart(args.progressStart),
        progressEnd(args.progressEnd),
        timingSink(std::move(args.timingSink)),
        warningSink(std::move(args.warningSink)),
        enableSubtitlesShared(args.enableSubtitlesShared),
        windowTitle(args.windowTitle),
        file(args.file),
        quitApplicationRequested(args.quitApplicationRequested),
        systemControls(args.systemControls),
        requestTransportCommand(std::move(args.requestTransportCommand)),
        continuityState(args.continuityState),
        enableAscii(args.enableAscii),
        enableAudio(args.enableAudio),
        hasSubtitles(args.hasSubtitles),
        output(initialPlaybackLayout(args.config, args.continuityState),
               args.continuityState
                   ? std::optional<PlaybackSessionContinuationState>(
                         *args.continuityState)
                   : std::nullopt),
        core({args.player, args.perfLog, args.enableAudio, args.enableAscii}),
        gpuRenderer(sharedGpuRenderer()) {
    if (args.continuityState) {
      pictureInPictureStartedFromTerminal =
          args.continuityState->windowPlacement
              .pictureInPictureStartedFromTerminal;
    }
    core.initialize(screen);
    bindInputState();
    bindRenderInputs();
    applyPresenterSync(syncPresentation());
  }

  void bindInputState() {
    inputView.screen = &screen;
    inputView.videoWindow = &output.window();
    inputView.subtitleManager = &subtitleManager;
    inputView.windowTitle = &windowTitle;
    inputView.enableSubtitlesShared = &enableSubtitlesShared;
    inputView.hasSubtitles = hasSubtitles;
    inputView.currentMode = output.renderMode(enableAscii);
    inputView.frameOutputState = &frameOutputState;
    inputView.pictureInPictureTextOutputState =
        &pictureInPictureTextOutputState;
    inputView.timingSink = timingSink;
    core.bindInputView(inputView);

    inputSignals.overlayControlHover = &overlayControlHover;
    inputSignals.requestWindowPresent = [this]() {
      output.requestWindowPresent();
    };
    inputSignals.togglePictureInPicture = [this]() {
      return this->togglePictureInPicture();
    };
    inputSignals.requestTransportCommand = [this](PlaybackTransportCommand cmd) {
      if (!requestTransportCommand) {
        return false;
      }
      return requestTransportCommand(cmd);
    };
    inputSignals.overlayUntilMs = &overlayUntilMs;
    inputSignals.desiredLayout = &output.desiredLayout();
    inputSignals.loopStopRequested = &loopStopRequested;
    inputSignals.quitApplicationRequested = quitApplicationRequested;
    inputSignals.redraw = &redraw;
    inputSignals.forceRefreshArt = &forceRefreshArt;
    core.bindSeekState(seekState);
  }

  void bindRenderInputs() {
    renderInputs.screen = &screen;
    renderInputs.videoWindow = &output.window();
    renderInputs.subtitleManager = &subtitleManager;
    renderInputs.gpuRenderer = &gpuRenderer;
    renderInputs.frameCache = &output.frameCache();
    renderInputs.art = &art;
    renderInputs.windowTitle = &windowTitle;
    renderInputs.baseStyle = &baseStyle;
    renderInputs.accentStyle = &accentStyle;
    renderInputs.dimStyle = &dimStyle;
    renderInputs.progressEmptyStyle = &progressEmptyStyle;
    renderInputs.progressFrameStyle = &progressFrameStyle;
    renderInputs.progressStart = &progressStart;
    renderInputs.progressEnd = &progressEnd;
    renderInputs.enableSubtitlesShared = &enableSubtitlesShared;
    renderInputs.overlayControlHover = &overlayControlHover;
    renderInputs.frameOutputState = &frameOutputState;
    renderInputs.warningSink = warningSink;
    renderInputs.timingSink = timingSink;
    core.bindRenderInputs(renderInputs);
  }

  bool overlayVisible() const {
    return playback_session_input::isOverlayVisible(inputSignals);
  }

  WindowUiState buildWindowUiState() {
    return playback_framebuffer_presenter::buildPlaybackFramebufferUiState(
        windowTitle, output.window(), core.player(), subtitleManager,
        core.playbackState(), core.audioOk(), requestTransportCommand != nullptr,
        requestTransportCommand != nullptr, hasSubtitles,
        enableSubtitlesShared, *seekState.windowLocalSeekRequested,
        *seekState.windowPendingSeekTargetSec, overlayControlHover,
        overlayVisible());
  }

  bool togglePictureInPicture() {
    VideoWindow& window = output.window();
    if (window.IsOpen() && window.IsPictureInPicture()) {
      pendingPictureInPictureOpen = false;
      pendingPictureInPictureTextMode = false;
      window.SetPictureInPictureTextMode(false);
      if (pictureInPictureStartedFromTerminal) {
        pictureInPictureStartedFromTerminal = false;
        output.requestLayout(PlaybackLayout::Terminal);
        forceRefreshArt = true;
        redraw = true;
        return true;
      }
      pictureInPictureStartedFromTerminal = false;
      redraw = true;
      output.requestWindowPresent();
      return window.SetPictureInPicture(false);
    }

    const bool fromTerminalAscii = !output.windowActive() && enableAscii;
    const bool audioOnlyPlayback =
        core.player().sourceWidth() <= 0 || core.player().sourceHeight() <= 0;
    const bool textMode = fromTerminalAscii || audioOnlyPlayback;
    pendingPictureInPictureOpen = true;
    pendingPictureInPictureTextMode = textMode;
    pictureInPictureStartedFromTerminal = fromTerminalAscii;
    output.requestLayout(PlaybackLayout::Window);
    forceRefreshArt = true;
    redraw = true;

    if (window.IsOpen()) {
      window.SetPictureInPictureTextMode(textMode);
      if (window.SetPictureInPicture(true)) {
        pendingPictureInPictureOpen = false;
      }
    }
    output.requestWindowPresent();
    return true;
  }

  bool buildPictureInPictureTextGrid(int pixelWidth, int pixelHeight,
                                     int cellPixelWidth, int cellPixelHeight,
                                     const VideoFrame* frame,
                                     bool frameChanged,
                                     std::vector<ScreenCell>& outCells,
                                     int& outCols, int& outRows) {
    const int cols = playback_overlay::overlayCellCountForPixels(
        pixelWidth, cellPixelWidth);
    const int rows = playback_overlay::overlayCellCountForPixels(
        pixelHeight, cellPixelHeight);
    pictureInPictureTextScreen.setVirtualSize(cols, rows);

    pictureInPictureTextOutputState.renderFailed = false;
    pictureInPictureTextOutputState.renderFailMessage.clear();
    pictureInPictureTextOutputState.renderFailDetail.clear();
    if (frame && frame->width > 0 && frame->height > 0) {
      pictureInPictureTextFrame = *frame;
      pictureInPictureTextOutputState.haveFrame = true;
    } else if (!pictureInPictureTextOutputState.haveFrame) {
      pictureInPictureTextFrame = VideoFrame{};
    }

    playback_screen_renderer::PlaybackScreenRenderInputs inputs = renderInputs;
    inputs.screen = &pictureInPictureTextScreen;
    inputs.frame = &pictureInPictureTextFrame;
    inputs.frameCache = &pictureInPictureTextFrameCache;
    inputs.art = &pictureInPictureTextArt;
    inputs.currentMode = PlaybackRenderMode::AsciiTerminal;
    inputs.windowActive = false;
    inputs.useWindowPresenter = false;
    const bool audioOnlyPlayback =
        core.player().sourceWidth() <= 0 || core.player().sourceHeight() <= 0;
    inputs.overlayVisibleNow = overlayVisible() || audioOnlyPlayback;
    inputs.clearHistory = false;
    inputs.frameChanged = frameChanged;
    inputs.cellPixelWidth = cellPixelWidth;
    inputs.cellPixelHeight = cellPixelHeight;
    inputs.cellPixelSourceLabel = "picture-in-picture-text-grid";
    inputs.allowAsciiCpuFallback = false;
    inputs.frameOutputState = &pictureInPictureTextOutputState;
    core.updateRenderInputs(inputs);

    playback_screen_renderer::renderPlaybackScreen(inputs);
    if (pictureInPictureTextOutputState.renderFailed) {
      return false;
    }
    return pictureInPictureTextScreen.snapshot(outCells, outCols, outRows);
  }

  PlaybackPresenterSyncResult syncPresentation() {
    auto buildUiState = [&]() { return buildWindowUiState(); };
    auto buildPictureInPictureTextGrid =
        [&](int pixelWidth, int pixelHeight, int cellPixelWidth,
            int cellPixelHeight, const VideoFrame* frame, bool frameChanged,
            std::vector<ScreenCell>& outCells, int& outCols, int& outRows) {
          return this->buildPictureInPictureTextGrid(
              pixelWidth, pixelHeight, cellPixelWidth, cellPixelHeight, frame,
              frameChanged, outCells, outCols, outRows);
        };
    auto overlayVisibleFn = [&]() { return overlayVisible(); };
    PlaybackPresenterSyncResult result =
        output.sync(core.player(), buildUiState, overlayVisibleFn,
                    buildPictureInPictureTextGrid, redraw, forceRefreshArt,
                    overlayUntilMs, overlayControlHover);
    if (pendingPictureInPictureOpen && output.window().IsOpen()) {
      output.window().SetPictureInPictureTextMode(
          pendingPictureInPictureTextMode);
      if (output.window().SetPictureInPicture(true)) {
        pendingPictureInPictureOpen = false;
      }
      output.requestWindowPresent();
    } else if (!output.windowRequested()) {
      pendingPictureInPictureOpen = false;
      pendingPictureInPictureTextMode = false;
      pictureInPictureStartedFromTerminal = false;
    }
    return result;
  }

  void applyPresenterSync(const PlaybackPresenterSyncResult& syncResult) {
    core.applyPresenterSync(syncResult);
  }

  void shutdown() {
    perfLogAppendf(&perfLog, "video_shutdown begin");
    perfLogFlush(&perfLog);
    perfLogAppendf(&perfLog, "video_shutdown output_stop_begin");
    perfLogFlush(&perfLog);
    output.stop();
    perfLogAppendf(&perfLog, "video_shutdown output_stop_end");
    perfLogFlush(&perfLog);
    perfLogAppendf(&perfLog, "video_shutdown player_close_begin");
    perfLogFlush(&perfLog);
    core.shutdownPlayer();
    perfLogAppendf(&perfLog, "video_shutdown player_close_end");
    perfLogFlush(&perfLog);
    perfLogAppendf(&perfLog, "video_shutdown audio_stop_begin");
    perfLogFlush(&perfLog);
    core.shutdownAudio();
    perfLogAppendf(&perfLog, "video_shutdown audio_stop_end");
    perfLogFlush(&perfLog);
  }

  PlaybackSessionContinuationState buildContinuationState() {
    PlaybackSessionContinuationState state;
    state.hasLayout = true;
    state.layout = output.desiredLayout();
    state.windowPlacement.pictureInPictureStartedFromTerminal =
        pictureInPictureStartedFromTerminal;
    if (output.window().IsOpen()) {
      RECT windowRect{};
      if (output.window().GetWindowBounds(&windowRect)) {
        state.windowPlacement.hasWindowRect = true;
        state.windowPlacement.windowRect = windowRect;
      }
      state.windowPlacement.pictureInPictureActive =
          output.window().IsPictureInPicture();
      state.windowPlacement.pictureInPictureTextMode =
          output.window().IsPictureInPictureTextMode();
      if (state.windowPlacement.pictureInPictureActive) {
        if (output.window().GetPictureInPictureRestoreBounds(
                &state.windowPlacement.pictureInPictureRestoreRect)) {
          state.windowPlacement.hasPictureInPictureRestoreRect = true;
        }
        if (output.window().GetWindowBounds(
                &state.windowPlacement.pictureInPictureRect)) {
          state.windowPlacement.hasPictureInPictureRect = true;
        }
      }
    }
    return state;
  }

  void finalizeAudioStart() {
    if (core.finalizeAudioStart()) {
      redraw = true;
    }
  }

  void updateRenderInputs(bool clearHistory, bool frameChanged) {
    renderInputs.debugOverlay = config.debugOverlay;
    renderInputs.currentMode = output.renderMode(enableAscii);
    inputView.currentMode = renderInputs.currentMode;
    renderInputs.enableAudio = enableAudio;
    renderInputs.canPlayPrevious = requestTransportCommand != nullptr;
    renderInputs.canPlayNext = requestTransportCommand != nullptr;
    renderInputs.windowActive = output.windowActive();
    renderInputs.hasSubtitles = hasSubtitles;
    renderInputs.allowAsciiCpuFallback = false;
    renderInputs.useWindowPresenter = output.windowActive();
    renderInputs.overlayVisibleNow = overlayVisible();
    renderInputs.cellPixelWidth = screen.cellPixelWidth();
    renderInputs.cellPixelHeight = screen.cellPixelHeight();
    renderInputs.cellPixelSourceLabel = screen.cellPixelSourceLabel();
    renderInputs.clearHistory = clearHistory;
    renderInputs.frameChanged = frameChanged;
    core.updateRenderInputs(renderInputs);
  }

  void renderPlaybackFrame(bool presented, PlaybackLoopState& loopState) {
    auto t0 = std::chrono::steady_clock::now();
    updateRenderInputs(forceRefreshArt, presented);
    output.renderTerminal(renderInputs);
    auto t1 = std::chrono::steady_clock::now();
    lastOverlayRefresh = t1;
    lastDebugRefresh = t1;
    auto durMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    if (durMs > 100) {
      perfLogAppendf(&perfLog, "video_ui_draw_slow dur_ms=%lld",
                     static_cast<long long>(durMs));
    }
    if (frameOutputState.renderFailed) {
      loopState = PlaybackLoopState::Stopped;
      return;
    }
    redraw = false;
    forceRefreshArt = false;
  }

  bool initialize() {
    bool useWindowPresenter = output.windowActive();
    const auto now = std::chrono::steady_clock::now();
    lastOverlayRefresh = now;
    lastDebugRefresh = now;
    if (!useWindowPresenter) {
      updateRenderInputs(true, true);
      output.renderTerminal(renderInputs);
    } else {
      redraw = false;
      forceRefreshArt = false;
    }
    return !frameOutputState.renderFailed;
  }

  void pollWindowEvents() {
    if (output.consumeWindowCloseRequested() ||
        (output.windowRequested() && !output.windowVisible())) {
      pendingPictureInPictureOpen = false;
      pendingPictureInPictureTextMode = false;
      pictureInPictureStartedFromTerminal = false;
      output.window().SetPictureInPictureTextMode(false);
      output.requestLayout(PlaybackLayout::Terminal);
      forceRefreshArt = true;
      redraw = true;
      applyPresenterSync(syncPresentation());
    }
  }

  void emitHeartbeat() {
    auto nowUi = std::chrono::steady_clock::now();
    if (nowUi - lastUiHeartbeat < std::chrono::seconds(1)) {
      return;
    }
    const bool isPaused =
        core.playbackState() == PlaybackSessionState::Paused || audioIsPaused();
    const bool seeking =
        seekState.localSeekRequested && *seekState.localSeekRequested;
    perfLogAppendf(&perfLog,
                   "video_heartbeat_ui redraw=%d seeker=%d paused=%d",
                   redraw ? 1 : 0, seeking ? 1 : 0, isPaused ? 1 : 0);
    lastUiHeartbeat = nowUi;
  }

  bool pollNextEvent(InputEvent& ev) { return output.pollInput(input, ev); }

  void processInputEvents(PlaybackLoopState& loopState) {
    input.setCellPixelSize(screen.cellPixelWidth(), screen.cellPixelHeight());
    inputView.currentMode = output.renderMode(enableAscii);
    InputEvent ev{};
    while (pollNextEvent(ev)) {
      if (loopState == PlaybackLoopState::Stopped) {
        break;
      }
      if (ev.type == InputEvent::Type::Resize) {
        core.markPendingResize();
        redraw = true;
        applyPresenterSync(syncPresentation());
        continue;
      }
      if (ev.type == InputEvent::Type::Key) {
        playback_session_input::handlePlaybackKeyEvent(inputView, inputSignals,
                                                       seekState, ev.key);
        if (loopStopRequested) {
          break;
        }
        applyPresenterSync(syncPresentation());
        continue;
      }
      if (ev.type == InputEvent::Type::Mouse) {
        playback_session_input::handlePlaybackMouseEvent(inputView, inputSignals,
                                                         seekState, ev.mouse);
        if (loopStopRequested) {
          break;
        }
        applyPresenterSync(syncPresentation());
      }
    }
    if (loopStopRequested) {
      loopState = PlaybackLoopState::Stopped;
    }
  }

  void processSystemCommands(PlaybackLoopState& loopState) {
    if (!systemControls) {
      return;
    }
    PlaybackControlCommand command;
    while (systemControls->pollCommand(&command)) {
      playback_session_input::handlePlaybackControlCommand(
          inputView, inputSignals, seekState, command);
      if (loopStopRequested) {
        break;
      }
      applyPresenterSync(syncPresentation());
    }
    if (loopStopRequested) {
      loopState = PlaybackLoopState::Stopped;
    }
  }

  void updateSystemControls() {
    if (!systemControls) {
      return;
    }

    PlaybackSystemControls::State state;
    state.active = true;
    state.isVideo = true;
    state.file = file;
    state.trackIndex = -1;
    state.canPlay = true;
    state.canPause = true;
    state.canStop = true;
    state.canPrevious = requestTransportCommand != nullptr;
    state.canNext = requestTransportCommand != nullptr;

    const PlaybackSessionState playbackState = core.playbackState();
    if (playbackState == PlaybackSessionState::Ended) {
      state.status = PlaybackSystemControls::Status::Stopped;
    } else if (playbackState == PlaybackSessionState::Paused ||
               core.player().state() == PlayerState::Paused) {
      state.status = PlaybackSystemControls::Status::Paused;
    } else {
      state.status = PlaybackSystemControls::Status::Playing;
    }

    if (core.player().currentUs() > 0) {
      state.positionSec =
          static_cast<double>(core.player().currentUs()) / 1000000.0;
    }
    if (core.player().durationUs() > 0) {
      state.durationSec =
          static_cast<double>(core.player().durationUs()) / 1000000.0;
    }

    systemControls->update(state);
  }

  void updateWindowCursor() {
    output.updateWindowCursor(core.player(), core.playbackState(),
                              overlayVisible());
  }

  void flushQueuedSeek() {
    if (!seekState.seekQueued || !*seekState.seekQueued) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    bool canSend =
        (*seekState.lastSeekSentTime ==
             std::chrono::steady_clock::time_point::min()) ||
        (now - *seekState.lastSeekSentTime >= kSeekThrottleInterval);
    if (canSend) {
      playback_session_input::sendSeekRequest(inputView, inputSignals, seekState,
                                              *seekState.queuedSeekTargetSec);
    }
  }

  void handlePendingResize() {
    core.handlePendingResize(screen, output.renderMode(enableAscii), redraw);
  }

  struct RefreshState {
    bool useWindowPresenter = false;
    bool presented = false;
    bool overlayRefreshDue = false;
    bool debugRefreshDue = false;
  };

  int computeWaitTimeoutMs(const RefreshState& refresh) const {
    int timeoutMs = 250;
    const auto now = std::chrono::steady_clock::now();

    if (!refresh.useWindowPresenter && overlayVisible()) {
      auto overlayDue = lastOverlayRefresh + std::chrono::milliseconds(100);
      timeoutMs = std::min(
          timeoutMs,
          std::max(
              0, static_cast<int>(std::chrono::duration_cast<
                                       std::chrono::milliseconds>(overlayDue -
                                                                  now)
                                       .count())));
    }
    if (!refresh.useWindowPresenter && config.debugOverlay) {
      auto debugDue = lastDebugRefresh + std::chrono::milliseconds(250);
      timeoutMs = std::min(
          timeoutMs,
          std::max(
              0, static_cast<int>(std::chrono::duration_cast<
                                       std::chrono::milliseconds>(debugDue -
                                                                  now)
                                       .count())));
    }
    if (seekState.seekQueued && *seekState.seekQueued &&
        seekState.lastSeekSentTime) {
      auto seekDue = *seekState.lastSeekSentTime + kSeekThrottleInterval;
      timeoutMs = std::min(
          timeoutMs,
          std::max(
              0, static_cast<int>(std::chrono::duration_cast<
                                       std::chrono::milliseconds>(seekDue - now)
                                       .count())));
    }
    if (!refresh.useWindowPresenter &&
        core.playbackState() == PlaybackSessionState::Active) {
      timeoutMs = std::min(timeoutMs, 16);
    }
    return std::max(0, timeoutMs);
  }

  void waitForNextActivity(const RefreshState& refresh) {
    if (loopStopRequested || redraw || refresh.overlayRefreshDue ||
        refresh.debugRefreshDue) {
      return;
    }

    const int timeoutMs = computeWaitTimeoutMs(refresh);
    if (timeoutMs <= 0) {
      return;
    }

    if (!refresh.useWindowPresenter &&
        core.playbackState() == PlaybackSessionState::Active) {
      output.waitForActivity(input, timeoutMs, core.videoFrameWaitHandle());
      return;
    }

    output.waitForActivity(input, timeoutMs);
  }

  RefreshState refreshState() {
    RefreshState state;
    state.useWindowPresenter = output.windowActive();
    state.presented =
        core.refresh(state.useWindowPresenter, output.windowActive(), redraw);
    const bool overlayVisibleNow = overlayVisible();
    const auto nowForRefresh = std::chrono::steady_clock::now();
    state.overlayRefreshDue =
        !state.useWindowPresenter && overlayVisibleNow &&
        (lastOverlayRefresh == std::chrono::steady_clock::time_point::min() ||
         nowForRefresh - lastOverlayRefresh >= std::chrono::milliseconds(100));
    state.debugRefreshDue =
        !state.useWindowPresenter && config.debugOverlay &&
        (lastDebugRefresh == std::chrono::steady_clock::time_point::min() ||
         nowForRefresh - lastDebugRefresh >= std::chrono::milliseconds(250));
    return state;
  }

  void renderFailureScreen() {
    updateRenderInputs(true, true);
    output.renderTerminal(renderInputs);
  }

  void run() {
    if (!initialize()) {
      capturedContinuationState = buildContinuationState();
      return;
    }

    PlaybackLoopState loopState = PlaybackLoopState::Running;
    while (loopState == PlaybackLoopState::Running) {
      finalizeAudioStart();
      updateSystemControls();
      pollWindowEvents();

      emitHeartbeat();
      processSystemCommands(loopState);
      if (loopState == PlaybackLoopState::Stopped) {
        break;
      }
      processInputEvents(loopState);
      if (loopState == PlaybackLoopState::Stopped) {
        break;
      }

      applyPresenterSync(syncPresentation());
      finalizeAudioStart();
      updateWindowCursor();
      flushQueuedSeek();
      handlePendingResize();

      RefreshState refresh = refreshState();
      if (shouldRenderPlaybackFrame(redraw, refresh.presented,
                                    refresh.overlayRefreshDue,
                                    refresh.debugRefreshDue,
                                    core.playbackState())) {
        renderPlaybackFrame(refresh.presented, loopState);
        if (frameOutputState.renderFailed) {
          break;
        }
      } else if (refresh.useWindowPresenter) {
        redraw = false;
        forceRefreshArt = false;
      }

      waitForNextActivity(refresh);
    }
    capturedContinuationState = buildContinuationState();
  }
};

PlaybackLoopRunner::PlaybackLoopRunner(Args args)
    : impl_(std::make_unique<Impl>(std::move(args))) {}

PlaybackLoopRunner::~PlaybackLoopRunner() = default;

PlaybackLoopRunner::PlaybackLoopRunner(PlaybackLoopRunner&&) noexcept = default;

PlaybackLoopRunner& PlaybackLoopRunner::operator=(
    PlaybackLoopRunner&&) noexcept = default;

void PlaybackLoopRunner::run() { impl_->run(); }

void PlaybackLoopRunner::shutdown() { impl_->shutdown(); }

void PlaybackLoopRunner::renderFailureScreen() { impl_->renderFailureScreen(); }

PlaybackSessionContinuationState PlaybackLoopRunner::continuationState() const {
  return impl_->capturedContinuationState;
}

bool PlaybackLoopRunner::hasRenderFailure() const {
  return impl_->frameOutputState.renderFailed;
}

const std::string& PlaybackLoopRunner::renderFailureMessage() const {
  return impl_->frameOutputState.renderFailMessage;
}

const std::string& PlaybackLoopRunner::renderFailureDetail() const {
  return impl_->frameOutputState.renderFailDetail;
}
