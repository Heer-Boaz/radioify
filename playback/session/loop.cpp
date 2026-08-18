#include "loop.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "asciiart.h"
#include "asciiart_gpu.h"
#include "audioplayback.h"
#include "playback/video/gpu/gpu_shared.h"
#include "playback/video/player.h"
#include "playback/video/state/machine.h"
#include "playback/video/timeline_preview.h"
#include "playback/video/timeline_preview_model.h"
#include "playback/ascii/frame_output.h"
#include "playback/ascii/screen_renderer.h"
#include "playback/framebuffer/presenter.h"
#include "playback/session/osd_timeline.h"
#include "playback/session/context_menu_controller.h"
#include "playback/session/video_edit_workspace.h"
#include "playback_mode.h"
#include "playback/notification_area/controls.h"
#include "playback/system_media_transport/controls.h"
#include "core/windows_console_window.h"
#include "core/open_file_requests.h"
#include "core/runtime_helpers.h"
#include "core.h"
#include "handoff.h"
#include "input.h"
#include "output.h"
#include "presentation.h"
#include "presentation_controller.h"
#include "state.h"
#include "playback/video/subtitle/manager.h"

namespace {

enum class PlaybackLoopState : uint8_t {
  Running,
  Stopped,
};

bool shouldRenderPlaybackFrame(bool redraw, bool presented,
                               bool debugRefreshDue,
                               PlaybackSessionState playbackState) {
  return redraw || presented ||
         (debugRefreshDue && playbackState != PlaybackSessionState::Ended);
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
  static constexpr auto kFrameCopyMessageDuration =
      std::chrono::milliseconds(1500);
  static constexpr auto kEditMessageDuration = std::chrono::milliseconds(2200);

  struct PendingExit {
    enum class Kind : uint8_t {
      Session,
      QuitApplication,
      Transport,
      OpenFiles,
      DeferredOpenRequest,
    };

    Kind kind = Kind::Session;
    PlaybackTransportCommand transport = PlaybackTransportCommand::Next;
    std::vector<std::filesystem::path> files;
    std::optional<OpenFilesRequest> deferredOpenRequest;
    bool exitWhenExportSucceeds = false;
    bool resumePlaybackOnCancel = false;
  };

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
  OpenFileRequests& openFileRequests;
  bool* quitApplicationRequested = nullptr;
  PlaybackSystemControls* systemControls = nullptr;
  PlaybackNotificationAreaControls* notificationAreaControls = nullptr;
  std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
  std::function<bool(const std::vector<std::filesystem::path>&)> requestOpenFiles;
  PlaybackSessionContinuationState* continuityState = nullptr;
  const PlaybackSessionIntent sessionIntent;
  PlaybackSessionContinuationState capturedContinuationState;
  const bool enableAscii;
  const bool enableAudio;
  const bool hasSubtitles;

  PlaybackOutputController output;
  PlaybackPresentationController presentationController;
  PlaybackSessionCore core;
  GpuAsciiRenderer& gpuRenderer;
  AsciiArt art;
  playback_screen_renderer::TimelinePreviewAsciiCache timelinePreviewArt;
  ConsoleScreen textGridPresentationScreen;
  std::vector<ScreenCell> textGridPresentationCells;
  AsciiArt textGridPresentationArt;
  playback_screen_renderer::TimelinePreviewAsciiCache
      textGridTimelinePreviewArt;
  VideoFrame textGridPresentationFrame;
  GpuVideoFrameCache textGridPresentationFrameCache;
  playback_frame_output::FrameOutputState textGridPresentationOutputState;
  bool copiedFrameNeedsRender = false;
  bool redraw = true;
  bool forceRefreshArt = false;
  playback_frame_output::FrameOutputState frameOutputState;
  playback_session::PlaybackOsdTimeline osd;
  playback_video_timeline_preview::HoverModel timelinePreviewModel;
  playback_video_timeline_preview::Provider timelinePreviewProvider;
  bool timelinePreviewStarted = false;
  playback_session::VideoEditWorkspace videoEditWorkspace;
  playback_session::ContextMenuController contextMenuController;
  mutable std::mutex publishedWindowUiMutex;
  playback_framebuffer_presenter::PlaybackFramebufferUiSnapshot
      publishedWindowUi;
  std::optional<PendingExit> pendingExit;
  std::atomic<int> overlayControlHover{-1};
  bool loopStopRequested = false;
  std::chrono::steady_clock::time_point lastDebugRefresh =
      std::chrono::steady_clock::time_point::min();
  std::chrono::steady_clock::time_point lastUiHeartbeat =
      std::chrono::steady_clock::now();

  playback_session_input::PlaybackInputView inputView;
  playback_session_input::PlaybackInputSignals inputSignals;
  playback_session_input::PlaybackSeekGestureState seekState;
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
        openFileRequests(args.openFileRequests),
        quitApplicationRequested(args.quitApplicationRequested),
        systemControls(args.systemControls),
        notificationAreaControls(args.notificationAreaControls),
        requestTransportCommand(std::move(args.requestTransportCommand)),
        requestOpenFiles(std::move(args.requestOpenFiles)),
        continuityState(args.continuityState),
        sessionIntent(args.sessionIntent),
        enableAscii(args.enableAscii),
        enableAudio(args.enableAudio),
        hasSubtitles(args.hasSubtitles),
        output(initialPlaybackLayout(args.config, args.continuityState),
               args.continuityState
                   ? std::optional<PlaybackSessionContinuationState>(
                         *args.continuityState)
                   : std::nullopt),
        presentationController(args.continuityState),
        core({args.player, args.perfLog, args.enableAudio, args.enableAscii}),
        gpuRenderer(sharedGpuRenderer()),
        videoEditWorkspace(args.file, core.player(), timelinePreviewModel,
                           timelinePreviewProvider) {
    core.initialize(screen);
    const playback_video_timeline_preview::Source previewSource{
        file, core.player().videoStreamIndex(), core.player().durationUs(),
        core.player().sourceWidth(), core.player().sourceHeight()};
    timelinePreviewModel.start(previewSource.durationUs,
                               previewSource.sourceWidth,
                               previewSource.sourceHeight);
    timelinePreviewStarted = timelinePreviewProvider.start(previewSource);
    if (!timelinePreviewStarted) timelinePreviewModel.stop();
    syncVideoEditPresentation(false);
    bindInputState();
    bindRenderInputs();
    if (sessionIntent == PlaybackSessionIntent::EditVideo) {
      executeVideoEditCommand(playback_video_edit::Command::Open, false);
    }
    applyPresenterSync(syncPresentation());
  }

  void showEditMessage(const std::string& message) {
    osd.showMessage(message,
                    playback_session::PlaybackOsdTimeline::Clock::now(),
                    kEditMessageDuration);
    redraw = true;
    publishWindowUiState();
    output.requestWindowPresent();
  }

  void finishLoopExit(bool quitApplication) {
    if (inputView.playbackState) {
      *inputView.playbackState = PlaybackSessionState::Exiting;
    }
    loopStopRequested = true;
    osd.clear();
    redraw = true;
    forceRefreshArt = true;
    if (quitApplication && quitApplicationRequested) {
      *quitApplicationRequested = true;
    }
  }

  bool exitNeedsConfirmation() const {
    return videoEditWorkspace.needsExitConfirmation();
  }

  playback_video_edit::Prompt videoEditPrompt() const {
    if (pendingExit) return playback_video_edit::Prompt::LeavePlayback;
    return videoEditWorkspace.prompt();
  }

  bool beginPendingExit(PendingExit request) {
    if (!exitNeedsConfirmation()) return true;
    if (!pendingExit) {
      if (videoEditWorkspace.prompt() != playback_video_edit::Prompt::None) {
        videoEditWorkspace.execute(playback_video_edit::Command::CancelPrompt);
      }
      request.resumePlaybackOnCancel =
          inputView.playbackState &&
          *inputView.playbackState == PlaybackSessionState::Active;
      pendingExit = std::move(request);
      overlayControlHover.store(-1, std::memory_order_relaxed);
      playback_session_input::setPlaybackPaused(inputView, inputSignals,
                                                seekState, true);
      syncVideoEditPresentation();
    }
    return false;
  }

  void requestPlaybackExit(bool quitApplication) {
    PendingExit request;
    request.kind = quitApplication ? PendingExit::Kind::QuitApplication
                                   : PendingExit::Kind::Session;
    if (beginPendingExit(std::move(request))) {
      finishLoopExit(quitApplication);
    }
  }

  bool requestTransportExit(PlaybackTransportCommand command) {
    if (!requestTransportCommand) return false;
    PendingExit request;
    request.kind = PendingExit::Kind::Transport;
    request.transport = command;
    if (!beginPendingExit(std::move(request))) return false;
    return requestTransportCommand(command);
  }

  bool requestOpenFilesExit(
      const std::vector<std::filesystem::path>& files) {
    if (!requestOpenFiles) return false;
    PendingExit request;
    request.kind = PendingExit::Kind::OpenFiles;
    request.files = files;
    if (!beginPendingExit(std::move(request))) return false;
    return requestOpenFiles(files);
  }

  bool completePendingExit() {
    if (!pendingExit || videoEditWorkspace.exitContext().exportRunning) {
      return false;
    }
    PendingExit request = std::move(*pendingExit);
    pendingExit.reset();
    overlayControlHover.store(-1, std::memory_order_relaxed);
    bool accepted = true;
    switch (request.kind) {
      case PendingExit::Kind::Session:
        finishLoopExit(false);
        break;
      case PendingExit::Kind::QuitApplication:
        finishLoopExit(true);
        break;
      case PendingExit::Kind::Transport:
        accepted = requestTransportCommand &&
                   requestTransportCommand(request.transport);
        if (accepted) finishLoopExit(false);
        break;
      case PendingExit::Kind::OpenFiles:
        accepted = requestOpenFiles && requestOpenFiles(request.files);
        if (accepted) finishLoopExit(false);
        break;
      case PendingExit::Kind::DeferredOpenRequest:
        if (request.deferredOpenRequest) {
          openFileRequests.post(std::move(*request.deferredOpenRequest));
          finishLoopExit(false);
        } else {
          accepted = false;
        }
        break;
    }
    if (!accepted) {
      if (request.resumePlaybackOnCancel) {
        playback_session_input::setPlaybackPaused(inputView, inputSignals,
                                                  seekState, false);
      }
      syncVideoEditPresentation();
      showEditMessage("Could not complete the requested playback change");
    }
    return accepted;
  }

  bool cancelPendingExit() {
    if (!pendingExit) return false;
    const bool resumePlayback = pendingExit->resumePlaybackOnCancel;
    pendingExit.reset();
    overlayControlHover.store(-1, std::memory_order_relaxed);
    if (resumePlayback) {
      playback_session_input::setPlaybackPaused(inputView, inputSignals,
                                                seekState, false);
    }
    syncVideoEditPresentation();
    showEditMessage("Exit cancelled; edits retained");
    return true;
  }

  void navigateBack() {
    if (pendingExit) {
      cancelPendingExit();
      return;
    }
    const playback_session::VideoEditActionResult result =
        videoEditWorkspace.navigateBack();
    if (!result.handled) {
      requestPlaybackExit(false);
      return;
    }
    overlayControlHover.store(-1, std::memory_order_relaxed);
    syncVideoEditPresentation();
    if (!result.message.empty()) showEditMessage(result.message);
  }

  void publishWindowUiState() {
    playback_framebuffer_presenter::PlaybackFramebufferUiSnapshot next;
    next.osd = osdSnapshot();
    next.timelinePreview = timelinePreviewModel.snapshotFor(
        playback_video_timeline_preview::PresentationSurface::VideoWindow);
    next.videoEdit = videoEditWorkspace.edit();
    next.videoEditExport = videoEditWorkspace.exportProgress();
    next.videoEditPrompt = videoEditPrompt();
    next.contextMenu = contextMenuController.snapshotFor(
        playback_session::ContextMenuSurface::VideoWindow);
    std::lock_guard<std::mutex> lock(publishedWindowUiMutex);
    publishedWindowUi = std::move(next);
  }

  playback_framebuffer_presenter::PlaybackFramebufferUiSnapshot
  windowUiStateSnapshot() const {
    std::lock_guard<std::mutex> lock(publishedWindowUiMutex);
    return publishedWindowUi;
  }

  void syncVideoEditPresentation(bool requestPresent = true) {
    contextMenuController.refresh(videoEditWorkspace.edit(),
                                  videoEditWorkspace.exportProgress());
    if (videoEditPrompt() != playback_video_edit::Prompt::None) {
      contextMenuController.dismiss();
      timelinePreviewModel.hide(
          playback_video_timeline_preview::PresentationSurface::Terminal);
      timelinePreviewModel.hide(
          playback_video_timeline_preview::PresentationSurface::VideoWindow);
      timelinePreviewProvider.cancelBefore(timelinePreviewModel.requestId());
    }
    publishWindowUiState();
    if (!requestPresent) return;
    redraw = true;
    output.requestWindowPresent();
  }

  void pollVideoEditExport() {
    const playback_session::VideoEditPollResult result =
        videoEditWorkspace.poll();
    if (!result.changed) return;
    overlayControlHover.store(-1, std::memory_order_relaxed);
    syncVideoEditPresentation();
    if (!result.message.empty()) showEditMessage(result.message);
    if (pendingExit && pendingExit->exitWhenExportSucceeds &&
        result.completion !=
            playback_session::VideoEditExportCompletion::None) {
      pendingExit->exitWhenExportSucceeds = false;
      if (result.completion ==
              playback_session::VideoEditExportCompletion::Succeeded &&
          !videoEditWorkspace.hasUnexportedChanges()) {
        completePendingExit();
      }
    }
  }

  void pollVideoEditBoundaryCommit() {
    if (!seekState.pendingVideoEditBoundaryCommit) return;
    if (!videoEditWorkspace.active() || pendingExit ||
        videoEditWorkspace.prompt() != playback_video_edit::Prompt::None) {
      seekState.pendingVideoEditBoundaryCommit.reset();
      return;
    }

    const auto pending = *seekState.pendingVideoEditBoundaryCommit;
    const PlayerTimelineSnapshot timeline = core.player().timelineSnapshot();
    switch (playback_session_input::videoEditBoundaryCommitState(
        pending.seekGeneration, timeline.latestSeekRequestGeneration,
        timeline.handledSeekRequestGeneration, timeline.seekPending(),
        timeline.frameDurationUs > 0)) {
      case playback_session_input::VideoEditBoundaryCommitState::Waiting:
        return;
      case playback_session_input::VideoEditBoundaryCommitState::Superseded:
        // Never attach an edit mark to a frame from the wrong seek
        // generation.
        seekState.pendingVideoEditBoundaryCommit.reset();
        return;
      case playback_session_input::VideoEditBoundaryCommitState::Ready:
        break;
    }

    seekState.pendingVideoEditBoundaryCommit.reset();
    const playback_video_edit::Command command =
        pending.boundary == playback_video_edit::EditBoundary::In
            ? playback_video_edit::Command::MarkIn
            : playback_video_edit::Command::MarkOut;
    executeVideoEditCommand(command, false);
  }

  bool executeVideoEditCommand(playback_video_edit::Command command,
                               bool announce = true) {
    seekState.pendingVideoEditBoundaryCommit.reset();
    const bool startForPendingExit =
        pendingExit &&
        command == playback_video_edit::Command::StartExport &&
        playback_video_edit::exitExportAction(
            videoEditWorkspace.exitContext()) ==
            playback_video_edit::ExitExportAction::ExportCurrent;
    const playback_session::VideoEditActionResult result =
        videoEditWorkspace.execute(command);
    if (result.pausePlayback) {
      playback_session_input::setPlaybackPaused(inputView, inputSignals,
                                                seekState, true);
    }
    overlayControlHover.store(-1, std::memory_order_relaxed);
    std::string message = result.message;
    syncVideoEditPresentation();
    if (startForPendingExit) {
      const playback_video_edit::ExitContext context =
          videoEditWorkspace.exitContext();
      if (result.exportStarted) {
        pendingExit->exitWhenExportSucceeds = true;
        message = "Exporting; will exit after success";
      } else if (!context.hasUnexportedChanges && !context.exportRunning) {
        return completePendingExit();
      }
    }
    if (!message.empty() && (announce || !videoEditWorkspace.active())) {
      showEditMessage(message);
    }
    return result.handled;
  }

  bool waitForVideoEditExportAndExit() {
    if (!pendingExit) return false;
    const playback_video_edit::ExitExportAction action =
        playback_video_edit::exitExportAction(
            videoEditWorkspace.exitContext());
    if (action != playback_video_edit::ExitExportAction::WaitForExport) {
      return false;
    }
    // Arm the exact rendered intent before inspecting the worker again. A
    // short export may reach a terminal state between the click and this UI
    // turn; the next poll must still resolve that completion as Wait requested.
    pendingExit->exitWhenExportSucceeds = true;
    syncVideoEditPresentation();
    showEditMessage("Will exit after export succeeds");
    return true;
  }

  bool handleContextMenuInput(
      const playback_session::ContextMenuInput& request) {
    bool handled = false;
    std::optional<playback_video_edit::Command> activatedCommand;
    using InputKind = playback_session::ContextMenuInputKind;
    switch (request.kind) {
      case InputKind::Open: {
        if (videoEditPrompt() != playback_video_edit::Prompt::None) {
          return false;
        }
        if (videoEditWorkspace.active()) {
          if (request.timelineUs) {
            videoEditWorkspace.selectCutAt(
                *request.timelineUs, request.timelineToleranceUs);
          } else {
            videoEditWorkspace.clearCutSelection();
          }
          syncVideoEditPresentation(false);
        }
        const int width =
            request.surface == playback_session::ContextMenuSurface::Terminal
                ? screen.width()
                : output.window().GetWidth();
        const int height =
            request.surface == playback_session::ContextMenuSurface::Terminal
                ? screen.height()
                : output.window().GetHeight();
        const double xRatio =
            request.x / static_cast<double>(std::max(1, width - 1));
        const double yRatio =
            request.y / static_cast<double>(std::max(1, height - 1));
        handled = contextMenuController.open(
            request.surface, xRatio, yRatio);
        if (handled) {
          const auto previewSurface =
              request.surface ==
                      playback_session::ContextMenuSurface::Terminal
                  ? playback_video_timeline_preview::PresentationSurface::Terminal
                  : playback_video_timeline_preview::PresentationSurface::VideoWindow;
          timelinePreviewModel.hide(previewSurface);
          timelinePreviewProvider.cancelBefore(timelinePreviewModel.requestId());
        }
        break;
      }
      case InputKind::Dismiss:
        handled = contextMenuController.dismiss();
        break;
      case InputKind::MoveSelection:
        if (contextMenuController.visible()) {
          contextMenuController.moveSelection(request.selectionDelta);
          handled = true;
        }
        break;
      case InputKind::SelectItem:
        if (contextMenuController.visible() && request.item) {
          contextMenuController.select(*request.item);
          handled = true;
        }
        break;
      case InputKind::ActivateSelection:
        if (contextMenuController.visible()) {
          activatedCommand = contextMenuController.activateSelection();
          handled = true;
        }
        break;
      case InputKind::ActivateItem:
        if (contextMenuController.visible() && request.item) {
          activatedCommand = contextMenuController.activate(*request.item);
          handled = true;
        }
        break;
    }
    if (activatedCommand) {
      executeVideoEditCommand(*activatedCommand);
    }
    if (handled) {
      redraw = true;
      publishWindowUiState();
      output.requestWindowPresent();
    }
    return handled;
  }

  void bindInputState() {
    inputView.videoWindow = &output.window();
    inputView.subtitleManager = &subtitleManager;
    inputView.enableSubtitlesShared = &enableSubtitlesShared;
    inputView.hasSubtitles = hasSubtitles;
    inputView.frameOutputState = &frameOutputState;
    inputView.timingSink = timingSink;
    core.bindInputView(inputView);

    inputSignals.overlayControlHover = &overlayControlHover;
    inputSignals.requestWindowPresent = [this]() {
      publishWindowUiState();
      output.requestWindowPresent();
    };
    inputSignals.copyCurrentVideoFrameToClipboard = [this]() {
      std::string error;
      if (!output.copyCurrentVideoFrameToClipboard(&error)) {
        std::fprintf(stderr, "Copy frame failed: %s\n", error.c_str());
        osd.showMessage("Frame copy failed",
                        playback_session::PlaybackOsdTimeline::Clock::now(),
                        kFrameCopyMessageDuration);
      } else {
        osd.showMessage(
            "Frame copied to clipboard",
            playback_session::PlaybackOsdTimeline::Clock::now(),
            kFrameCopyMessageDuration);
      }
      redraw = true;
      publishWindowUiState();
      output.requestWindowPresent();
    };
    inputSignals.videoEditorActive =
        [this]() { return videoEditWorkspace.active(); };
    inputSignals.videoEditPrompt = [this]() { return videoEditPrompt(); };
    inputSignals.executeVideoEditCommand =
        [this](playback_video_edit::Command command) {
          return executeVideoEditCommand(command);
        };
    inputSignals.waitForVideoEditExportAndExit =
        [this]() { return waitForVideoEditExportAndExit(); };
    inputSignals.navigateBack = [this]() { navigateBack(); };
    inputSignals.confirmPendingExit =
        [this]() { return completePendingExit(); };
    inputSignals.cancelPendingExit =
        [this]() { return cancelPendingExit(); };
    inputSignals.contextMenuVisible =
        [this]() { return contextMenuController.visible(); };
    inputSignals.handleContextMenuInput =
        [this](const playback_session::ContextMenuInput& request) {
          return handleContextMenuInput(request);
        };
    inputSignals.moveVideoEditBoundary =
        [this](playback_video_edit::EditBoundary boundary,
               int64_t timelineUs) {
          if (pendingExit ||
              !videoEditWorkspace.moveBoundary(boundary, timelineUs)) {
            return false;
          }
          syncVideoEditPresentation();
          return true;
        };
    inputSignals.requestTimelinePreview =
        [this](playback_video_timeline_preview::PresentationSurface surface,
               double ratio, int progressUnits) {
          auto update =
              timelinePreviewModel.hover(surface, ratio, progressUnits);
          if (update.request &&
              !timelinePreviewProvider.submit(*update.request)) {
            timelinePreviewModel.reject(*update.request);
          }
          if (update.changed) {
            redraw = true;
            publishWindowUiState();
            output.requestWindowPresent();
          }
        };
    inputSignals.clearTimelinePreview =
        [this](playback_video_timeline_preview::PresentationSurface surface) {
          if (timelinePreviewModel.hide(surface)) {
            timelinePreviewProvider.cancelBefore(
                timelinePreviewModel.requestId());
            redraw = true;
            publishWindowUiState();
            output.requestWindowPresent();
          }
        };
    inputSignals.toggleWindowPresentation = [this]() {
      return presentationController.toggleWindow(output, redraw,
                                                 forceRefreshArt);
    };
    inputSignals.togglePictureInPicture = [this]() {
      const bool audioOnlyPlayback =
          core.player().sourceWidth() <= 0 || core.player().sourceHeight() <= 0;
      return presentationController.togglePictureInPicture(
          output, enableAscii, audioOnlyPlayback, redraw, forceRefreshArt);
    };
    inputSignals.toggleFullscreen = [this]() {
      return presentationController.toggleFullscreen(output, enableAscii, redraw,
                                                     forceRefreshArt);
    };
    inputSignals.requestPlaybackExit =
        [this](bool quitApplication) {
          requestPlaybackExit(quitApplication);
        };
    inputSignals.requestTransportCommand =
        [this](PlaybackTransportCommand cmd) {
          return requestTransportExit(cmd);
        };
    inputSignals.requestOpenFiles =
        [this](const std::vector<std::filesystem::path>& files) {
      return requestOpenFilesExit(files);
    };
    inputSignals.osd = &osd;
    inputSignals.loopStopRequested = &loopStopRequested;
    inputSignals.redraw = &redraw;
    inputSignals.forceRefreshArt = &forceRefreshArt;
  }

  void bindRenderInputs() {
    renderInputs.screen = &screen;
    renderInputs.videoWindow = &output.window();
    renderInputs.subtitleManager = &subtitleManager;
    renderInputs.gpuRenderer = &gpuRenderer;
    renderInputs.frameCache = &output.frameCache();
    renderInputs.art = &art;
    renderInputs.timelinePreviewCache = &timelinePreviewArt;
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
    renderInputs.videoEdit = videoEditWorkspace.edit();
    renderInputs.videoEditExport = videoEditWorkspace.exportProgress();
    renderInputs.videoEditPrompt = videoEditPrompt();
    renderInputs.contextMenu = contextMenuController.snapshotFor(
        playback_session::ContextMenuSurface::Terminal);
    core.bindRenderInputs(renderInputs);
  }

  bool overlayVisible() const {
    return config.debugOverlay || osd.controlsVisible() ||
           videoEditWorkspace.active() || pendingExit.has_value() ||
           videoEditWorkspace.exportProgress().running() ||
           contextMenuController.visible();
  }

  playback_overlay::PlaybackOsdSnapshot osdSnapshot() const {
    playback_overlay::PlaybackOsdSnapshot snapshot = osd.snapshot();
    snapshot.controlsVisible = snapshot.controlsVisible || config.debugOverlay;
    return snapshot;
  }

  WindowUiState buildWindowUiState() {
    const auto snapshot = windowUiStateSnapshot();
    return playback_framebuffer_presenter::buildPlaybackFramebufferUiState(
        windowTitle, output.window(), core.player(), subtitleManager,
        core.playbackState(), core.audioOk(),
        requestTransportCommand != nullptr,
        requestTransportCommand != nullptr, hasSubtitles,
        enableSubtitlesShared, overlayControlHover, snapshot,
        config.debugOverlay);
  }

  bool buildTextGridPresentation(int pixelWidth, int pixelHeight,
                                 int cellPixelWidth, int cellPixelHeight,
                                 const VideoFrame* frame, bool frameChanged,
                                 const std::string& enhancementDebugLine,
                                 std::vector<ScreenCell>& outCells,
                                 int& outCols, int& outRows,
                                 playback_overlay::InteractionMap&
                                     outInteractions) {
    const int cols = playback_overlay::overlayCellCountForPixels(
        pixelWidth, cellPixelWidth);
    const int rows = playback_overlay::overlayCellCountForPixels(
        pixelHeight, cellPixelHeight);
    textGridPresentationScreen.setVirtualSize(cols, rows);

    textGridPresentationOutputState.renderFailed = false;
    textGridPresentationOutputState.renderFailMessage.clear();
    textGridPresentationOutputState.renderFailDetail.clear();
    if (frame && frame->width > 0 && frame->height > 0) {
      textGridPresentationFrame = *frame;
      textGridPresentationOutputState.haveFrame = true;
    } else if (!textGridPresentationOutputState.haveFrame) {
      textGridPresentationFrame = VideoFrame{};
    }

    playback_screen_renderer::PlaybackScreenRenderInputs inputs = renderInputs;
    inputs.screen = &textGridPresentationScreen;
    inputs.frame = &textGridPresentationFrame;
    inputs.frameCache = &textGridPresentationFrameCache;
    inputs.art = &textGridPresentationArt;
    inputs.timelinePreviewCache = &textGridTimelinePreviewArt;
    inputs.currentMode = PlaybackRenderMode::AsciiTerminal;
    inputs.windowActive = false;
    inputs.useWindowPresenter = false;
    const bool audioOnlyPlayback =
        core.player().sourceWidth() <= 0 || core.player().sourceHeight() <= 0;
    const auto windowUi = windowUiStateSnapshot();
    inputs.osd = windowUi.osd;
    inputs.timelinePreview = windowUi.timelinePreview;
    inputs.videoEdit = windowUi.videoEdit;
    if (inputs.videoEdit.active) {
      inputs.videoEdit.playheadTimelineUs =
          core.player().timelineSnapshot().positionUs;
    }
    inputs.videoEditExport = windowUi.videoEditExport;
    inputs.videoEditPrompt = windowUi.videoEditPrompt;
    inputs.contextMenu = windowUi.contextMenu;
    inputs.osd.controlsVisible =
        inputs.osd.controlsVisible || audioOnlyPlayback;
    inputs.clearHistory = false;
    inputs.frameChanged = frameChanged;
    inputs.cellPixelWidth = cellPixelWidth;
    inputs.cellPixelHeight = cellPixelHeight;
    inputs.cellPixelSourceLabel = "text-grid-presentation";
    inputs.allowAsciiCpuFallback = false;
    if (config.debugOverlay) {
      inputs.debugLines.push_back(output.window().OutputColorDebugLine());
      if (!enhancementDebugLine.empty()) {
        inputs.debugLines.push_back(enhancementDebugLine);
      }
    }
    inputs.frameOutputState = &textGridPresentationOutputState;
    core.updateRenderInputs(inputs);
    inputs.frameAvailable = textGridPresentationOutputState.haveFrame;

    playback_screen_renderer::renderPlaybackScreen(inputs);
    if (textGridPresentationOutputState.renderFailed) {
      return false;
    }
    outInteractions = textGridPresentationOutputState.overlayInteractions;
    return textGridPresentationScreen.snapshot(outCells, outCols, outRows);
  }

  PlaybackPresenterSyncResult syncPresentation() {
    auto buildUiState = [&]() { return buildWindowUiState(); };
    auto buildTextGridPresentation =
        [&](int pixelWidth, int pixelHeight, int cellPixelWidth,
            int cellPixelHeight, const VideoFrame* frame, bool frameChanged,
            const std::string& enhancementDebugLine,
            std::vector<ScreenCell>& outCells, int& outCols, int& outRows,
            playback_overlay::InteractionMap& outInteractions) {
          return this->buildTextGridPresentation(
              pixelWidth, pixelHeight, cellPixelWidth, cellPixelHeight, frame,
              frameChanged, enhancementDebugLine, outCells, outCols, outRows,
              outInteractions);
        };
    PlaybackPresenterSyncResult result =
        output.sync(core.player(), buildUiState, buildTextGridPresentation,
                    redraw, forceRefreshArt);
    presentationController.reconcile(output);
    return result;
  }

  void applyPresenterSync(const PlaybackPresenterSyncResult& syncResult) {
    if (syncResult.switchedAwayFromWindow() || syncResult.windowStartFailed) {
      osd.clearControls();
      overlayControlHover.store(-1, std::memory_order_relaxed);
    }
    if (core.applyPresenterSync(syncResult)) {
      copiedFrameNeedsRender = true;
      forceRefreshArt = true;
      redraw = true;
    }
  }

  void shutdown() {
    perfLogAppendf(&perfLog, "video_shutdown begin");
    perfLogFlush(&perfLog);
    timelinePreviewProvider.stop();
    timelinePreviewModel.stop();
    timelinePreviewStarted = false;
    videoEditWorkspace.stop();
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
    state.asciiRenderingEnabled = enableAscii;
    presentationController.captureWindowPlacement(output, state);
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
    renderInputs.enableAudio = enableAudio;
    renderInputs.canPlayPrevious = requestTransportCommand != nullptr;
    renderInputs.canPlayNext = requestTransportCommand != nullptr;
    renderInputs.windowActive = output.windowActive();
    renderInputs.hasSubtitles = hasSubtitles;
    renderInputs.allowAsciiCpuFallback = false;
    renderInputs.useWindowPresenter = output.windowActive();
    renderInputs.osd = osdSnapshot();
    renderInputs.timelinePreview = timelinePreviewModel.snapshotFor(
        playback_video_timeline_preview::PresentationSurface::Terminal);
    renderInputs.videoEdit = videoEditWorkspace.edit();
    renderInputs.videoEditExport = videoEditWorkspace.exportProgress();
    renderInputs.videoEditPrompt = videoEditPrompt();
    renderInputs.contextMenu = contextMenuController.snapshotFor(
        playback_session::ContextMenuSurface::Terminal);
    renderInputs.cellPixelWidth = screen.cellPixelWidth();
    renderInputs.cellPixelHeight = screen.cellPixelHeight();
    renderInputs.cellPixelSourceLabel = screen.cellPixelSourceLabel();
    renderInputs.clearHistory = clearHistory;
    renderInputs.frameChanged = frameChanged;
    core.updateRenderInputs(renderInputs);
  }

  void renderPlaybackFrame(bool presented, PlaybackLoopState& loopState) {
    auto t0 = std::chrono::steady_clock::now();
    const bool renderCopiedFrame = copiedFrameNeedsRender;
    updateRenderInputs(forceRefreshArt || renderCopiedFrame,
                       presented || renderCopiedFrame);
    output.renderTerminal(renderInputs);
    auto t1 = std::chrono::steady_clock::now();
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
    copiedFrameNeedsRender = false;
  }

  bool initialize() {
    bool useWindowPresenter = output.windowActive();
    const auto now = std::chrono::steady_clock::now();
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
      presentationController.handleWindowClosed(output, redraw,
                                                forceRefreshArt);
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
        seekState.seekQueued || core.player().timelineSnapshot().seekPending();
    perfLogAppendf(&perfLog,
                   "video_heartbeat_ui redraw=%d seeker=%d paused=%d",
                   redraw ? 1 : 0, seeking ? 1 : 0, isPaused ? 1 : 0);
    lastUiHeartbeat = nowUi;
  }

  bool pollNextEvent(InputEvent& ev) { return output.pollInput(input, ev); }

  void processOpenFileRequests(PlaybackLoopState& loopState) {
    if (!openFileRequests.hasPending() || pendingExit) return;
    if (exitNeedsConfirmation()) {
      OpenFilesRequest deferred;
      if (!openFileRequests.poll(deferred)) return;
      PendingExit request;
      request.kind = PendingExit::Kind::DeferredOpenRequest;
      request.deferredOpenRequest = std::move(deferred);
      beginPendingExit(std::move(request));
      return;
    }
    loopStopRequested = true;
    loopState = PlaybackLoopState::Stopped;
  }

  void processInputEvents(PlaybackLoopState& loopState) {
    input.setCellPixelSize(screen.cellPixelWidth(), screen.cellPixelHeight());
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
      if (ev.type == InputEvent::Type::FileDrop &&
          isCommittedFileDropEvent(ev.fileDrop)) {
        playback_session_handoff::requestOpenFilesHandoff(
            inputView, inputSignals, ev.fileDrop.files);
        if (loopStopRequested) {
          break;
        }
        applyPresenterSync(syncPresentation());
        continue;
      }
      if (ev.type == InputEvent::Type::Key ||
          ev.type == InputEvent::Type::Action) {
        playback_session_input::handlePlaybackInputEvent(
            inputView, inputSignals, seekState, ev);
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
        continue;
      }
      if (ev.type == InputEvent::Type::PointerLeave) {
        playback_session_input::handlePlaybackPointerLeave(
            inputSignals, seekState, inputView);
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

  void processNotificationAreaCommands(PlaybackLoopState& loopState) {
    if (!notificationAreaControls) {
      return;
    }

    PlaybackNotificationAreaCommand command;
    while (notificationAreaControls->pollCommand(&command)) {
      switch (command.kind) {
        case PlaybackNotificationAreaCommand::Kind::Activate:
          if (output.windowActive() && output.windowOpen()) {
            output.window().Activate();
          } else {
            activateWindowsConsoleWindow();
          }
          break;
        case PlaybackNotificationAreaCommand::Kind::Playback:
          playback_session_input::handlePlaybackControlCommand(
              inputView, inputSignals, seekState, command.playbackCommand);
          if (loopStopRequested) {
            break;
          }
          applyPresenterSync(syncPresentation());
          break;
        case PlaybackNotificationAreaCommand::Kind::Quit:
          requestPlaybackExit(true);
          break;
      }
      if (loopStopRequested) {
        break;
      }
    }
    if (loopStopRequested) {
      loopState = PlaybackLoopState::Stopped;
    }
  }

  PlaybackControlState buildVideoControlState() const {
    PlaybackControlState state;
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
      state.status = PlaybackControlStatus::Stopped;
    } else if (playbackState == PlaybackSessionState::Paused ||
               playback_video_state_machine::project(core.player().state())
                       .transport ==
                   playback_video_state_machine::TransportState::Paused) {
      state.status = PlaybackControlStatus::Paused;
    } else {
      state.status = PlaybackControlStatus::Playing;
    }

    const PlayerTimelineSnapshot timeline = core.player().timelineSnapshot();
    if (timeline.positionUs > 0) {
      state.positionSec =
          static_cast<double>(timeline.positionUs) / 1000000.0;
    }
    if (core.player().durationUs() > 0) {
      state.durationSec =
          static_cast<double>(core.player().durationUs()) / 1000000.0;
    }

    return state;
  }

  void updateSystemControls() {
    if (!systemControls) {
      return;
    }

    systemControls->update(buildVideoControlState());
  }

  void updateNotificationAreaControls() {
    if (!notificationAreaControls) {
      return;
    }

    notificationAreaControls->update(buildVideoControlState());
  }

  void updateWindowCursor() {
    output.updateWindowCursor(core.player(), core.playbackState(),
                              overlayVisible());
  }

  void flushQueuedSeek() {
    if (!seekState.seekQueued) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    bool canSend =
        (seekState.lastSeekSentTime ==
             std::chrono::steady_clock::time_point::min()) ||
        (now - seekState.lastSeekSentTime >= kSeekThrottleInterval);
    if (canSend) {
      playback_session_input::sendSeekRequest(inputView, inputSignals, seekState,
                                              seekState.queuedSeekTargetSec);
    }
  }

  void handlePendingResize() {
    core.handlePendingResize(screen, output.renderMode(enableAscii), redraw);
  }

  struct RefreshState {
    bool useWindowPresenter = false;
    bool presented = false;
    bool debugRefreshDue = false;
  };

  int computeWaitTimeoutMs(const RefreshState& refresh) const {
    int timeoutMs = 250;
    const auto now = std::chrono::steady_clock::now();
    const auto tightenToDeadline =
        [&](playback_session::PlaybackOsdTimeline::TimePoint deadline) {
      const auto remaining = deadline - now;
      const int candidateMs =
          remaining <= std::chrono::steady_clock::duration::zero()
              ? 0
              : static_cast<int>(
                    std::chrono::ceil<std::chrono::milliseconds>(remaining)
                        .count());
      timeoutMs = std::min(timeoutMs, candidateMs);
    };

    if (const auto osdDeadline = osd.nextDeadline()) {
      tightenToDeadline(*osdDeadline);
    }
    if (!refresh.useWindowPresenter && config.debugOverlay) {
      tightenToDeadline(lastDebugRefresh + std::chrono::milliseconds(250));
    }
    if (seekState.seekQueued) {
      tightenToDeadline(seekState.lastSeekSentTime + kSeekThrottleInterval);
    }
    if (!refresh.useWindowPresenter &&
        core.playbackState() == PlaybackSessionState::Active) {
      timeoutMs = std::min(timeoutMs, 16);
    }
    return std::max(0, timeoutMs);
  }

  void waitForNextActivity(const RefreshState& refresh) {
    if (loopStopRequested || redraw || refresh.debugRefreshDue) {
      return;
    }

    const int timeoutMs = computeWaitTimeoutMs(refresh);
    if (timeoutMs <= 0) {
      return;
    }

    if (!refresh.useWindowPresenter &&
        core.playbackState() == PlaybackSessionState::Active) {
      output.waitForActivity(
          input, timeoutMs, core.videoFrameWaitHandle(),
          openFileRequests.nativeWaitHandle(),
          notificationAreaControls
              ? notificationAreaControls->nativeWaitHandle()
              : NativeWaitHandle(),
          timelinePreviewStarted ? timelinePreviewProvider.changedWaitHandle()
                                 : NativeWaitHandle());
      return;
    }

    output.waitForActivity(
        input, timeoutMs, core.player().statusChangeWaitHandle(),
        openFileRequests.nativeWaitHandle(),
        notificationAreaControls ? notificationAreaControls->nativeWaitHandle()
                                 : NativeWaitHandle(),
        timelinePreviewStarted ? timelinePreviewProvider.changedWaitHandle()
                               : NativeWaitHandle());
  }

  RefreshState refreshState() {
    RefreshState state;
    state.useWindowPresenter = output.windowActive();
    state.presented =
        core.refresh(state.useWindowPresenter, output.windowActive(), redraw);
    const auto nowForRefresh = std::chrono::steady_clock::now();
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
      pollVideoEditExport();
      pollVideoEditBoundaryCommit();
      if (std::optional<playback_video_timeline_preview::Result> result =
              timelinePreviewProvider.takeResult()) {
        if (timelinePreviewModel.apply(*result)) {
          redraw = true;
          publishWindowUiState();
          output.requestWindowPresent();
        }
      }
      if (osd.expire(playback_session::PlaybackOsdTimeline::Clock::now())) {
        redraw = true;
        publishWindowUiState();
        output.requestWindowPresent();
      }
      finalizeAudioStart();
      updateSystemControls();
      updateNotificationAreaControls();
      pollWindowEvents();

      emitHeartbeat();
      processSystemCommands(loopState);
      if (loopState == PlaybackLoopState::Stopped) {
        break;
      }
      processNotificationAreaCommands(loopState);
      if (loopState == PlaybackLoopState::Stopped) {
        break;
      }
      processOpenFileRequests(loopState);
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
