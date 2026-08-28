#include "loop.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "asciiart.h"
#include "asciiart_gpu.h"
#include "audioplayback.h"
#include "playback/video/gpu/gpu_runtime.h"
#include "playback/video/player.h"
#include "playback/video/edit/overlay_model.h"
#include "playback/video/state/machine.h"
#include "playback/video/timeline_preview.h"
#include "playback/video/timeline_preview_model.h"
#include "playback/ascii/frame_output.h"
#include "playback/ascii/screen_renderer.h"
#include "playback/framebuffer/presenter.h"
#include "playback/debug/lines.h"
#include "playback/overlay/overlay.h"
#include "playback/session/osd_timeline.h"
#include "playback/session/context_menu_controller.h"
#include "playback/session/media_task_cancellation.h"
#include "playback/session/video_edit_workspace.h"
#include "core/windows_console_window.h"
#include "core/runtime_helpers.h"
#include "core.h"
#include "input.h"
#include "media_task_feedback.h"
#include "output.h"
#include "presentation_controller.h"
#include "presentation_model.h"
#include "presentation_projector.h"
#include "state.h"
#include "mouse_double_click_tracker.h"
#include "playback/video/subtitle/manager.h"

namespace {

using namespace std::chrono_literals;

constexpr auto kTerminalPlaybackRefreshInterval = 16ms;
constexpr auto kTerminalDebugRefreshInterval = 250ms;
constexpr auto kTimingLogHeartbeatInterval = 1s;

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

PlaybackPresentationState initialPlaybackPresentation(
    const VideoPlaybackConfig& config,
    const PlaybackSessionContinuationState& continuityState) {
  if (continuityState.presentation) {
    return *continuityState.presentation;
  }
  return config.enableAscii ? PlaybackPresentationState::terminalAscii()
                            : PlaybackPresentationState::nativeWindowed();
}

}  // namespace

struct PlaybackLoopRunner::Impl : playback_session_input::SessionPort {
  static constexpr auto kSeekThrottleInterval = std::chrono::milliseconds(50);
  static constexpr auto kFrameCopyMessageDuration =
      std::chrono::milliseconds(1500);
  static constexpr auto kEditMessageDuration = std::chrono::milliseconds(2200);

  ConsoleScreen& screen;
  AudioPlaybackRuntime& audioPlayback;
  GpuRuntime& gpu;
  const VideoPlaybackConfig config;
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
  bool subtitlesEnabled;
  const std::string windowTitle;
  const std::filesystem::path file;
  const playback_session::Capabilities capabilities;
  playback_media_processing::Actions mediaProcessingActions;
  const PlaybackSessionIntent sessionIntent;
  PlaybackSessionContinuationState capturedContinuationState;
  bool quitApplicationRequested = false;
  const bool enableAudio;
  bool hasSubtitles;
  int overlayControlHover = -1;

  PlaybackPresentationController presentationController;
  PlaybackSessionCore core;
  playback_screen_renderer::PlaybackScreenResources screenResources;
  std::shared_ptr<playback_session::PresentationModel> presentationModel;
  AsciiArt art;
  playback_screen_renderer::TimelinePreviewAsciiCache timelinePreviewArt;
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
  playback_session_exit::ExitCoordinator exitCoordinator;
  playback_session::MediaTaskCancellationPromptState
      mediaTaskCancellationPrompt;
  std::deque<std::vector<std::filesystem::path>> deferredNativeFileDrops;
  // Session transitions run on the owner thread and are drained by the
  // coordinator before it can wait again. Cross-thread activity has separate
  // native signals and must not publish through this outbox.
  std::vector<playback_session::Event> events;
  bool exitWhenExportSucceeds = false;
  bool loopStopRequested = false;
  bool externalInputModal = false;
  bool initialized = false;
  bool finished = false;
  std::chrono::steady_clock::time_point lastDebugRefresh =
      std::chrono::steady_clock::time_point::min();
  std::chrono::steady_clock::time_point lastUiHeartbeat =
      std::chrono::steady_clock::now();

  pointer_input::MouseDoubleClickTracker mouseDoubleClickTracker;
  playback_session_input::PlaybackSeekGestureState seekState;
  // Constructed last and therefore stopped first. The presenter cannot outlive
  // any session-owned state referenced by its published presentation model.
  PlaybackOutputController output;

  explicit Impl(PlaybackLoopRunner::Args args)
      : screen(args.screen),
        audioPlayback(args.audioPlayback),
        gpu(args.gpu),
        config(std::move(args.config)),
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
        subtitlesEnabled(args.subtitlesEnabled),
        windowTitle(std::move(args.windowTitle)),
        file(std::move(args.file)),
        capabilities(args.capabilities),
        mediaProcessingActions(std::move(args.mediaProcessingActions)),
        sessionIntent(args.sessionIntent),
        enableAudio(args.enableAudio),
        hasSubtitles(args.hasSubtitles),
        presentationController(
            initialPlaybackPresentation(config, args.continuityState),
            args.continuityState.windowPlacement),
        core({args.player, audioPlayback, args.perfLog, args.enableAudio,
              initialPlaybackPresentation(config, args.continuityState)
                  .usesAsciiGrid()}),
        screenResources{gpu,
                        baseStyle,
                        accentStyle,
                        dimStyle,
                        progressEmptyStyle,
                        progressFrameStyle,
                        progressStart,
                        progressEnd, warningSink,
                        timingSink},
        presentationModel(std::make_shared<playback_session::PresentationModel>(
            playback_session::PresentationModel::Dependencies{
                screenResources})),
        videoEditWorkspace(file, core.player(), timelinePreviewModel,
                           timelinePreviewProvider),
        output(args.player, gpu, windowTitle, presentationModel,
               config.systemMediaCommandOwner) {
    core.initialize(screen);
    const playback_video_timeline_preview::Source previewSource{
        file, core.player().videoStreamIndex(), core.player().durationUs(),
        core.player().sourceWidth(), core.player().sourceHeight()};
    timelinePreviewModel.start(previewSource.durationUs,
                               previewSource.sourceWidth,
                               previewSource.sourceHeight);
    timelinePreviewStarted = timelinePreviewProvider.start(previewSource);
    if (!timelinePreviewStarted) timelinePreviewModel.stop();
    syncOverlayPresentation(false);
    if (sessionIntent == PlaybackSessionIntent::EditVideo) {
      executeVideoEditCommand(playback_video_edit::Command::Open, false);
    }
    applyPresenterSync(syncPresentation());
  }

  ~Impl() = default;

  void showEditMessage(const std::string& message) {
    osd.showMessage(message,
                    playback_session::PlaybackOsdTimeline::Clock::now(),
                    kEditMessageDuration);
    redraw = true;
    publishWindowUiState();
    output.requestWindowPresent();
  }

  void finishLoopExit(bool quitApplication) {
    core.beginExit();
    loopStopRequested = true;
    osd.clear();
    redraw = true;
    forceRefreshArt = true;
    quitApplicationRequested = quitApplicationRequested || quitApplication;
  }

  bool exitNeedsConfirmation() const {
    return videoEditWorkspace.needsExitConfirmation();
  }

  playback_video_edit::Prompt videoEditPrompt() const {
    if (exitCoordinator.confirmationVisible()) {
      return playback_video_edit::Prompt::LeavePlayback;
    }
    return videoEditWorkspace.prompt();
  }

  bool applyExitTransition(
      const playback_session_exit::Transition& transition) {
    if (!transition.handled) return false;
    if (transition.handoffRequest) {
      events.emplace_back(*transition.handoffRequest);
    }
    if (transition.handoffCancellation) {
      events.emplace_back(*transition.handoffCancellation);
    }
    if (transition.resumePlayback) {
      playback_session_input::setPlaybackPaused(*this, seekState, false);
    }
    if (transition.finishSession) {
      finishLoopExit(transition.quitApplication);
    }
    return true;
  }

  playback_session_exit::Transition beginExit(
      playback_session_exit::Intent intent) {
    if (finished || exitCoordinator.pending()) return {};

    mediaTaskCancellationPrompt.dismiss();

    const bool confirmationRequired = exitNeedsConfirmation();
    if (confirmationRequired &&
        videoEditWorkspace.prompt() != playback_video_edit::Prompt::None) {
      videoEditWorkspace.execute(playback_video_edit::Command::CancelPrompt);
    }
    const bool playbackActive =
        core.playbackState() == PlaybackSessionState::Active;
    playback_session_exit::Transition transition = exitCoordinator.request(
        std::move(intent), confirmationRequired, playbackActive);
    if (transition.handled && exitCoordinator.confirmationVisible()) {
      exitWhenExportSucceeds = false;
      overlayControlHover = -1;
      playback_session_input::setPlaybackPaused(*this, seekState, true);
      syncOverlayPresentation();
    }
    applyExitTransition(transition);
    return transition;
  }

  void requestPlaybackExit(bool quitApplication) {
    beginExit(quitApplication
                  ? playback_session_exit::Intent{
                        playback_session_exit::QuitApplication{}}
                  : playback_session_exit::Intent{
                        playback_session_exit::StopSession{}});
  }

  bool requestTransportExit(PlaybackTransportCommand command) {
    if (!capabilities.transportHandoff) return false;
    return beginExit(playback_session_exit::Transport{command}).handled;
  }

  bool requestOpenFilesExit(
      const std::vector<std::filesystem::path>& files) {
    if (!capabilities.openFilesHandoff || files.empty()) return false;
    return beginExit(playback_session_exit::OpenFiles{files}).handled;
  }

  bool nativeFileDropAccepted() const {
    return !externalInputModal && !capturesBrowserInput() &&
           !exitCoordinator.pending();
  }

  bool receiveNativeFileDrop(std::vector<std::filesystem::path> files) {
    if (!capabilities.openFilesHandoff || files.empty()) return false;
    if (!nativeFileDropAccepted()) {
      deferredNativeFileDrops.push_back(std::move(files));
      return true;
    }
    return requestOpenFilesExit(files);
  }

  void resumeDeferredNativeFileDrop() {
    if (deferredNativeFileDrops.empty() || !nativeFileDropAccepted()) return;
    if (requestOpenFilesExit(deferredNativeFileDrops.front())) {
      deferredNativeFileDrops.pop_front();
    }
  }

  bool completePendingExit() {
    if (!exitCoordinator.confirmationVisible() ||
        videoEditWorkspace.exitContext().exportRunning) {
      return false;
    }
    exitWhenExportSucceeds = false;
    overlayControlHover = -1;
    const playback_session_exit::Transition transition =
        exitCoordinator.confirm();
    const bool handled = applyExitTransition(transition);
    if (handled && !transition.finishSession) {
      syncOverlayPresentation();
    }
    return handled;
  }

  bool cancelPendingExit() {
    const playback_session_exit::Transition transition =
        exitCoordinator.cancel();
    if (!transition.handled) return false;
    exitWhenExportSucceeds = false;
    overlayControlHover = -1;
    applyExitTransition(transition);
    syncOverlayPresentation();
    showEditMessage("Exit cancelled; edits retained");
    return true;
  }

  std::optional<playback_session_exit::RequestId> requestExternalHandoff() {
    // A modal task decision owns this playback surface. External open requests
    // must wait instead of silently dismissing the decision and replacing the
    // media underneath it.
    if (mediaTaskCancellationPrompt.snapshot()) return std::nullopt;
    const playback_session_exit::Transition transition =
        beginExit(playback_session_exit::ExternalHandoff{});
    return transition.handled ? transition.requestId : std::nullopt;
  }

  bool resolveHandoff(playback_session_exit::RequestId requestId,
                      bool accepted) {
    const playback_session_exit::Transition transition =
        exitCoordinator.resolve(requestId, accepted);
    if (!transition.handled) return false;
    exitWhenExportSucceeds = false;
    overlayControlHover = -1;
    applyExitTransition(transition);
    if (!accepted) {
      syncOverlayPresentation();
      showEditMessage("Could not complete the requested playback change");
    }
    return true;
  }

  void navigateBack() {
    if (exitCoordinator.confirmationVisible()) {
      cancelPendingExit();
      return;
    }
    const playback_session::VideoEditActionResult result =
        videoEditWorkspace.navigateBack();
    if (!result.handled) {
      requestPlaybackExit(false);
      return;
    }
    overlayControlHover = -1;
    syncOverlayPresentation();
    if (!result.message.empty()) showEditMessage(result.message);
  }

  void publishWindowUiState() {
    publishPresentation(buildScreenModel(false, false));
  }

  void syncOverlayPresentation(bool requestPresent = true) {
    playback_media_actions::Context sourceContext =
        mediaProcessingActions.contextForSource(file);
    sourceContext.currentPlayback = true;
    contextMenuController.refresh(videoEditWorkspace.edit(),
                                  videoEditWorkspace.exportProgress(),
                                  std::move(sourceContext));
    if (videoEditPrompt() != playback_video_edit::Prompt::None ||
        mediaTaskCancellationPrompt.snapshot()) {
      contextMenuController.dismiss();
      timelinePreviewModel.hide(
          playback_video_timeline_preview::PresentationSurface::Terminal);
      timelinePreviewModel.hide(
          playback_video_timeline_preview::PresentationSurface::VideoWindow);
      timelinePreviewProvider.cancelBefore(timelinePreviewModel.requestId());
    }
    output.window().SetFileDropAcceptanceEnabled(
        nativeFileDropAccepted());
    publishWindowUiState();
    if (!requestPresent) return;
    redraw = true;
    output.requestWindowPresent();
  }

  bool requestMediaTaskCancellation(
      playback_media_actions::Action action) {
    if (videoEditPrompt() != playback_video_edit::Prompt::None ||
        mediaTaskCancellationPrompt.snapshot()) {
      return false;
    }
    std::optional<playback_media_processing::CancellationRequest> request =
        mediaProcessingActions.prepareCancellation(action, file);
    if (!request) {
      syncOverlayPresentation();
      showEditMessage("The background task is no longer running");
      return true;
    }
    if (!mediaTaskCancellationPrompt.open(std::move(*request))) {
      return false;
    }
    overlayControlHover = -1;
    syncOverlayPresentation();
    return true;
  }

  bool resolveMediaTaskCancellation(
      std::optional<playback_session::MediaTaskCancellationChoice> choice) {
    std::optional<playback_session::MediaTaskCancellationActivation>
        activation = choice ? mediaTaskCancellationPrompt.resolve(*choice)
                            : mediaTaskCancellationPrompt.activate();
    if (!activation) return false;
    overlayControlHover = -1;
    seekState.overlayControlPointer.reset();
    if (activation->cancelTask) {
      events.emplace_back(playback_session::MediaTaskCancellationRequested{
          std::move(activation->request)});
    }
    syncOverlayPresentation();
    resumeDeferredNativeFileDrop();
    return true;
  }

  bool moveMediaTaskCancellationSelection(int direction) {
    if (!mediaTaskCancellationPrompt.moveSelection(direction)) return false;
    overlayControlHover = -1;
    syncOverlayPresentation();
    return true;
  }

  void pollVideoEditExport() {
    const playback_session::VideoEditPollResult result =
        videoEditWorkspace.poll();
    if (!result.changed) return;
    overlayControlHover = -1;
    syncOverlayPresentation();
    if (!result.message.empty()) showEditMessage(result.message);
    if (exitCoordinator.confirmationVisible() && exitWhenExportSucceeds &&
        result.completion !=
            playback_session::VideoEditExportCompletion::None) {
      exitWhenExportSucceeds = false;
      if (result.completion ==
              playback_session::VideoEditExportCompletion::Succeeded &&
          !videoEditWorkspace.hasUnexportedChanges()) {
        completePendingExit();
      }
    }
  }

  void pollVideoEditBoundaryCommit() {
    if (!seekState.pendingVideoEditBoundaryCommit) return;
    if (!videoEditWorkspace.active() || exitCoordinator.pending() ||
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
        exitCoordinator.confirmationVisible() &&
        command == playback_video_edit::Command::StartExport &&
        playback_video_edit::exitExportAction(
            videoEditWorkspace.exitContext()) ==
            playback_video_edit::ExitExportAction::ExportCurrent;
    const playback_session::VideoEditActionResult result =
        videoEditWorkspace.execute(command);
    if (result.pausePlayback) {
      playback_session_input::setPlaybackPaused(*this, seekState, true);
    }
    overlayControlHover = -1;
    std::string message = result.message;
    syncOverlayPresentation();
    if (startForPendingExit) {
      const playback_video_edit::ExitContext context =
          videoEditWorkspace.exitContext();
      if (result.exportStarted) {
        exitWhenExportSucceeds = true;
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

  bool executeMediaAction(playback_media_actions::Action action) {
    if (playback_media_processing::isCancellationAction(action)) {
      return requestMediaTaskCancellation(action);
    }
    const std::optional<playback_media_processing::ActionResult> processing =
        mediaProcessingActions.execute(action, file);
    if (processing) {
      syncOverlayPresentation();
      showEditMessage(processing->feedback);
      return true;
    }

    switch (action) {
      case playback_media_actions::Action::EditVideo:
        return executeVideoEditCommand(playback_video_edit::Command::Open);
      case playback_media_actions::Action::Play:
      case playback_media_actions::Action::BrowseTracks:
      case playback_media_actions::Action::AnalyzeAudio:
      case playback_media_actions::Action::SplitLoop:
        return false;
      case playback_media_actions::Action::GenerateSubtitles:
      case playback_media_actions::Action::CancelSubtitleGeneration:
      case playback_media_actions::Action::ExportTranscriptText:
      case playback_media_actions::Action::ExportAudio:
      case playback_media_actions::Action::CancelMediaExport:
      case playback_media_actions::Action::SeparateAudio:
      case playback_media_actions::Action::CancelAudioSeparation:
        return true;
    }
    return false;
  }

  bool executeContextMenuCommand(
      const playback_session::ContextMenuCommand& command) {
    if (const auto* media =
            std::get_if<playback_media_actions::Action>(&command)) {
      return executeMediaAction(*media);
    }
    return executeVideoEditCommand(
        std::get<playback_video_edit::Command>(command));
  }

  bool waitForVideoEditExportAndExit() {
    if (!exitCoordinator.confirmationVisible()) return false;
    const playback_video_edit::ExitExportAction action =
        playback_video_edit::exitExportAction(
            videoEditWorkspace.exitContext());
    if (action != playback_video_edit::ExitExportAction::WaitForExport) {
      return false;
    }
    // Arm the exact rendered intent before inspecting the worker again. A
    // short export may reach a terminal state between the click and this UI
    // turn; the next poll must still resolve that completion as Wait requested.
    exitWhenExportSucceeds = true;
    syncOverlayPresentation();
    showEditMessage("Will exit after export succeeds");
    return true;
  }

  bool handleContextMenuInput(
      const playback_session::ContextMenuInput& request) {
    bool handled = false;
    std::optional<playback_session::ContextMenuCommand> activatedCommand;
    using InputKind = playback_session::ContextMenuInputKind;
    switch (request.kind) {
      case InputKind::Open: {
        if (videoEditPrompt() != playback_video_edit::Prompt::None ||
            mediaTaskCancellationPrompt.snapshot()) {
          return false;
        }
        if (videoEditWorkspace.active()) {
          if (request.timelineUs) {
            videoEditWorkspace.selectCutAt(
                *request.timelineUs, request.timelineToleranceUs);
            videoEditWorkspace.selectSceneSuggestionAt(
                *request.timelineUs, request.timelineToleranceUs);
          } else {
            videoEditWorkspace.clearCutSelection();
          }
        }
        syncOverlayPresentation(false);
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
      executeContextMenuCommand(*activatedCommand);
    }
    if (handled) {
      redraw = true;
      publishWindowUiState();
      output.requestWindowPresent();
    }
    return handled;
  }

  bool dispatch(playback_session_input::Command command) override {
    return std::visit(
        [this](auto value) {
          return executeInputCommand(std::move(value));
        },
        std::move(command));
  }

  playback_session_input::SessionSnapshot snapshot() const override {
    const AudioPlaybackSnapshot audio = audioPlayback.snapshot();
    playback_session_input::SessionSnapshot state;
    state.transport = core.snapshot();
    state.audioDurationSec = audio.durationSec;
    state.audioSupports50HzToggle = audio.supports50HzToggle;
    state.pictureInPicture = output.window().IsPictureInPicture();
    state.videoEditorActive = videoEditWorkspace.active();
    state.videoEditPrompt = videoEditPrompt();
    state.mediaTaskCancellationPrompt =
        mediaTaskCancellationPrompt.snapshot().has_value();
    state.contextMenuVisible = contextMenuController.visible();
    state.playbackControlsVisible = osd.controlsVisible();
    state.stopRequested = loopStopRequested;
    return state;
  }

  playback_overlay::InteractionHit hitTest(
      const playback_session_input::InteractionRequest& request)
      const override {
    if (request.surface == playback_video_timeline_preview::
                               PresentationSurface::VideoWindow) {
      return output.window().OverlayHitAt(request.x, request.y,
                                          request.capturedProgress);
    }
    if (request.scaleX != 1.0 || request.scaleY != 1.0) {
      return playback_overlay::interactionHitAtTransformed(
          frameOutputState.overlayInteractions, 0.0, 0.0,
          request.scaleX, request.scaleY, request.x, request.y,
          request.capturedProgress);
    }
    return playback_overlay::interactionHitAt(
        frameOutputState.overlayInteractions, request.x, request.y,
        request.capturedProgress);
  }

  bool toggleSubtitles() {
    if (!hasSubtitles) return false;
    if (!subtitlesEnabled) {
      subtitleManager.selectFirstTrackWithCues();
      subtitlesEnabled = true;
      return true;
    }
    const size_t count = subtitleManager.selectableTrackCount();
    if (count <= 1 || subtitleManager.isActiveLastCueTrack()) {
      subtitlesEnabled = false;
      return true;
    }
    return subtitleManager.cycleLanguage();
  }

  bool executeInputCommand(playback_session_input::CommandAction action) {
    using Action = playback_session_input::CommandAction;
    switch (action) {
      case Action::RequestWindowPresent:
        publishWindowUiState();
        output.requestWindowPresent();
        return true;
      case Action::RequestRedraw:
        redraw = true;
        return true;
      case Action::RequestFrameRefresh:
        redraw = true;
        forceRefreshArt = true;
        return true;
      case Action::ToggleRadio:
        if (!core.snapshot().audioAvailable) return false;
        audioPlayback.cycleRadioFilter();
        return true;
      case Action::Toggle50Hz:
        if (!core.snapshot().audioAvailable ||
            !audioPlayback.snapshot().supports50HzToggle) {
          return false;
        }
        audioPlayback.toggle50Hz();
        return true;
      case Action::CycleAudioTrack:
        return core.cycleAudioTrack();
      case Action::ToggleSubtitles:
        return toggleSubtitles();
      case Action::ToggleWindowPresentation: {
        const bool changed = presentationController.toggleWindow();
        redraw = redraw || changed;
        forceRefreshArt = forceRefreshArt || changed;
        return changed;
      }
      case Action::TogglePictureInPicture: {
        const bool changed = presentationController.togglePictureInPicture();
        redraw = redraw || changed;
        forceRefreshArt = forceRefreshArt || changed;
        return changed;
      }
      case Action::ToggleFullscreen: {
        const bool changed = presentationController.toggleFullscreen();
        redraw = redraw || changed;
        forceRefreshArt = forceRefreshArt || changed;
        return changed;
      }
      case Action::CopyCurrentVideoFrame: {
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
        return true;
      }
      case Action::WaitForVideoEditExportAndExit:
        return waitForVideoEditExportAndExit();
      case Action::NavigateBack:
        navigateBack();
        return true;
      case Action::ConfirmPendingExit:
        return completePendingExit();
      case Action::CancelPendingExit:
        return cancelPendingExit();
      case Action::ConfirmMediaTaskCancellation:
        return resolveMediaTaskCancellation(
            playback_session::MediaTaskCancellationChoice::CancelTask);
      case Action::DismissMediaTaskCancellation:
        return resolveMediaTaskCancellation(
            playback_session::MediaTaskCancellationChoice::KeepRunning);
      case Action::ActivateSelectedMediaTaskCancellationAction:
        return resolveMediaTaskCancellation(std::nullopt);
      case Action::SelectPreviousMediaTaskCancellationAction:
        return moveMediaTaskCancellationSelection(-1);
      case Action::SelectNextMediaTaskCancellationAction:
        return moveMediaTaskCancellationSelection(1);
    }
    return false;
  }

  bool executeInputCommand(
      playback_session_input::TransportRequest request) {
    return requestTransportExit(request.command);
  }

  bool executeInputCommand(playback_session_input::VideoEditRequest request) {
    return executeVideoEditCommand(request.command);
  }

  bool executeInputCommand(playback_session_input::ContextMenuRequest request) {
    return handleContextMenuInput(request.input);
  }

  bool executeInputCommand(
      playback_session_input::MoveVideoEditBoundary request) {
    if (exitCoordinator.pending() ||
        !videoEditWorkspace.moveBoundary(request.boundary,
                                         request.timelineUs)) {
      return false;
    }
    syncOverlayPresentation();
    return true;
  }

  bool executeInputCommand(
      playback_session_input::PlaybackExitRequest request) {
    requestPlaybackExit(request.quitApplication);
    return true;
  }

  bool executeInputCommand(
      playback_session_input::TimelinePreviewRequest request) {
    auto update = timelinePreviewModel.hover(
        request.surface, request.ratio, request.progressUnits);
    if (update.request && !timelinePreviewProvider.submit(*update.request)) {
      timelinePreviewModel.reject(*update.request);
    }
    if (update.changed) {
      redraw = true;
      publishWindowUiState();
      output.requestWindowPresent();
    }
    return update.changed;
  }

  bool executeInputCommand(
      playback_session_input::ClearTimelinePreview request) {
    if (!timelinePreviewModel.hide(request.surface)) return false;
    timelinePreviewProvider.cancelBefore(timelinePreviewModel.requestId());
    redraw = true;
    publishWindowUiState();
    output.requestWindowPresent();
    return true;
  }

  bool executeInputCommand(
      playback_session_input::ShowPlaybackControls request) {
    osd.showControls(playback_session::PlaybackOsdTimeline::Clock::now(),
                     request.duration);
    redraw = true;
    return true;
  }

  bool executeInputCommand(
      playback_session_input::SetOverlayControlHover request) {
    if (overlayControlHover == request.token) return false;
    overlayControlHover = request.token;
    redraw = true;
    publishWindowUiState();
    output.requestWindowPresent();
    return true;
  }

  bool executeInputCommand(playback_session_input::SetPaused request) {
    core.setPaused(request.paused);
    return true;
  }

  bool executeInputCommand(playback_session_input::SeekTo request) {
    if (!core.seekTo(request.targetUs)) return false;
    if (timingSink) {
      char buf[192];
      std::snprintf(buf, sizeof(buf),
                    "seek_request target_sec=%.3f target_us=%lld",
                    static_cast<double>(request.targetUs) / 1000000.0,
                    static_cast<long long>(request.targetUs));
      timingSink(std::string(buf));
    }
    return true;
  }

  bool executeInputCommand(playback_session_input::SeekBy request) {
    if (!core.seekBy(request.deltaUs)) return false;
    if (timingSink) {
      const playback_session_input::TransportSnapshot transport =
          core.snapshot();
      char buf[256];
      std::snprintf(
          buf, sizeof(buf),
          "seek_relative_request delta_us=%lld target_us=%lld generation=%llu",
          static_cast<long long>(request.deltaUs),
          static_cast<long long>(transport.positionUs),
          static_cast<unsigned long long>(
              transport.latestSeekRequestGeneration));
      timingSink(std::string(buf));
    }
    return true;
  }

  bool executeInputCommand(playback_session_input::StepFrame request) {
    return core.requestFrameStep(request.direction);
  }

  bool executeInputCommand(playback_session_input::AdjustVolume request) {
    audioPlayback.adjustVolume(request.delta);
    return true;
  }

  bool overlayVisible() const {
    const playback_video_edit::EditSnapshot edit =
        videoEditWorkspace.edit();
    const playback_video_edit::ExportProgress editExport =
        videoEditWorkspace.exportProgress();
    return config.debugOverlay || osd.controlsVisible() ||
           playback_video_edit::needsOverlayPresentation(
               edit, editExport, videoEditPrompt()) ||
           contextMenuController.visible();
  }

  playback_overlay::PlaybackOsdSnapshot osdSnapshot() const {
    playback_overlay::PlaybackOsdSnapshot snapshot = osd.snapshot();
    snapshot.controlsVisible = snapshot.controlsVisible || config.debugOverlay;
    return snapshot;
  }

  PlaybackPresentationSyncResult syncPresentation() {
    return presentationController.synchronize(output);
  }

  void applyPresenterSync(const PlaybackPresentationSyncResult& syncResult) {
    if (syncResult.switchedAwayFromWindow() || syncResult.transitionFailed) {
      osd.clearControls();
      overlayControlHover = -1;
    }
    if (syncResult.transitionFailed) {
      osd.showMessage(
          syncResult.appliedState.requiresNativeWindow()
              ? "The requested presentation could not be applied."
              : "Native playback is unavailable; using terminal mode.",
          playback_session::PlaybackOsdTimeline::Clock::now(),
          kEditMessageDuration);
      publishWindowUiState();
      output.requestWindowPresent();
    }
    if (syncResult.visualModeChanged()) {
      core.setAsciiPresentation(screen, syncResult.appliedState.usesAsciiGrid());
    }
    if (syncResult.shellFocusTarget) {
      switch (*syncResult.shellFocusTarget) {
        case PlaybackShellFocusTarget::Browser:
          activateBrowser();
          break;
        case PlaybackShellFocusTarget::TerminalPlayback:
          activateWindowsConsoleWindow();
          break;
      }
    }
    if (core.applyPresentationSync(syncResult.switchedAwayFromWindow())) {
      copiedFrameNeedsRender = true;
      forceRefreshArt = true;
      redraw = true;
    }
  }

  void activateBrowser() {
    if (capabilities.browserSurfaceActivation) {
      events.emplace_back(
          playback_session::BrowserSurfaceActivationRequested{});
    } else {
      activateWindowsConsoleWindow();
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
    output.closeWindow();
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
    presentationController.captureWindowPlacement(output, state);
    return state;
  }

  void finalizeAudioStart() {
    if (core.finalizeAudioStart()) {
      redraw = true;
      publishWindowUiState();
      output.requestWindowPresent();
    }
  }

  playback_screen_renderer::PlaybackMediaPresentation
  captureMediaPresentation() const {
    const Player& player = core.player();
    const AudioPlaybackSnapshot audio = audioPlayback.snapshot();
    PlayerTimelineSnapshot timeline = player.timelineSnapshot();
    playback_overlay::SubtitlePresentation subtitle =
        playback_overlay::projectSubtitlePresentation(
            subtitleManager, subtitlesEnabled, timeline.seekPending(),
            timeline.sourcePositionUs, hasSubtitles);
    return playback_session::capturePlaybackMedia(
        player, audio, std::move(timeline), windowTitle, core.audioOk(),
        hasSubtitles, subtitlesEnabled, std::move(subtitle));
  }

  playback_overlay::PlaybackOverlayState buildOverlayState(
      const playback_screen_renderer::PlaybackMediaPresentation& media,
      PlaybackSessionState playbackState,
      playback_video_timeline_preview::PresentationSurface surface) const {
    playback_session::OverlayProjection projection{media};
    projection.playbackState = playbackState;
    projection.audioOk = core.audioOk();
    projection.canPlayPrevious = capabilities.transportHandoff;
    projection.canPlayNext = capabilities.transportHandoff;
    projection.osd = osdSnapshot();
    projection.pictureInPictureAvailable = true;
    projection.pictureInPictureActive =
        output.windowOpen() && output.window().IsPictureInPicture();
    projection.subtitleRenderError =
        output.window().GetSubtitleRenderError();
    projection.contextMenu = contextMenuController.snapshotFor(
        surface == playback_video_timeline_preview::PresentationSurface::Terminal
            ? playback_session::ContextMenuSurface::Terminal
            : playback_session::ContextMenuSurface::VideoWindow);
    projection.videoEdit = videoEditWorkspace.edit();
    projection.videoEditExport = videoEditWorkspace.exportProgress();
    projection.videoEditPrompt = videoEditPrompt();
    projection.mediaTaskCancellationPrompt =
        mediaTaskCancellationPrompt.snapshot();
    if (config.debugOverlay &&
        surface == playback_video_timeline_preview::
                       PresentationSurface::VideoWindow) {
      projection.debugLines.push_back(
          output.window().OutputColorDebugLine());
      projection.debugLines.push_back(
          playback_debug_lines::videoFrameDebugLine(media.debug));
    }
    return playback_session::projectPlaybackOverlay(std::move(projection));
  }

  playback_screen_renderer::PlaybackScreenModel buildScreenModel(
      bool clearHistory, bool frameChanged) {
    playback_screen_renderer::PlaybackScreenModel model;
    model.debugOverlay = config.debugOverlay;
    model.visualMode = presentationController.state().visual();
    model.enableAudio = enableAudio;
    model.nativeWindowActive = output.windowOpen();
    model.allowAsciiCpuFallback = false;
    model.media = captureMediaPresentation();
    model.timelinePreview = timelinePreviewModel.snapshotFor(
        playback_video_timeline_preview::PresentationSurface::Terminal);
    const PlaybackSessionPresentationSnapshot corePresentation =
        core.presentationSnapshot(model.nativeWindowActive);
    model.playbackState = corePresentation.playbackState;
    model.audioOk = corePresentation.audioOk;
    model.audioStarting = corePresentation.audioStarting;
    model.frameAvailable = corePresentation.frameAvailable;
    model.overlay = buildOverlayState(
        model.media, model.playbackState,
        playback_video_timeline_preview::PresentationSurface::Terminal);
    if (!overlayVisible()) overlayControlHover = -1;
    model.controlHoverToken = overlayControlHover;
    model.nativeWindowWidth = output.window().GetWidth();
    model.nativeWindowHeight = output.window().GetHeight();
    model.cellPixelWidth = screen.cellPixelWidth();
    model.cellPixelHeight = screen.cellPixelHeight();
    model.cellPixelSourceLabel = screen.cellPixelSourceLabel();
    model.clearHistory = clearHistory;
    model.frameChanged = frameChanged;
    return model;
  }

  void publishPresentation(
      const playback_screen_renderer::PlaybackScreenModel& model) {
    playback_session::PresentationModel::Revision revision;
    revision.textGrid = model;
    revision.textGrid.timelinePreview = timelinePreviewModel.snapshotFor(
        playback_video_timeline_preview::PresentationSurface::VideoWindow);
    revision.textGrid.overlay = buildOverlayState(
        model.media, model.playbackState,
        playback_video_timeline_preview::PresentationSurface::VideoWindow);
    revision.window =
        playback_session::projectWindowUiState(revision.textGrid);
    presentationModel->publish(std::move(revision));
  }

  void renderTerminal(
      const playback_screen_renderer::PlaybackScreenModel& model) {
    playback_screen_renderer::PlaybackScreenTarget target{
        screen, output.frameCache(), art, timelinePreviewArt,
        core.presentationFrame(), frameOutputState};
    playback_screen_renderer::renderPlaybackScreen(screenResources, target,
                                                   model);
  }

  void renderPlaybackFrame(bool presented, PlaybackLoopState& loopState) {
    if (presentationController.terminalRole() ==
        PlaybackShellTerminalRole::Browser) {
      publishPresentation(buildScreenModel(false, presented));
      redraw = false;
      forceRefreshArt = false;
      copiedFrameNeedsRender = false;
      return;
    }
    auto t0 = std::chrono::steady_clock::now();
    const bool renderCopiedFrame = copiedFrameNeedsRender;
    const playback_screen_renderer::PlaybackScreenModel model =
        buildScreenModel(forceRefreshArt || renderCopiedFrame,
                         presented || renderCopiedFrame);
    publishPresentation(model);
    renderTerminal(model);
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
    const bool nativeWindowActive = output.windowOpen();
    const auto now = std::chrono::steady_clock::now();
    lastDebugRefresh = now;
    if (!nativeWindowActive) {
      const playback_screen_renderer::PlaybackScreenModel model =
          buildScreenModel(true, true);
      publishPresentation(model);
      renderTerminal(model);
    } else {
      redraw = false;
      forceRefreshArt = false;
    }
    return !frameOutputState.renderFailed;
  }

  void pollWindowEvents() {
    if (output.consumeWindowCloseRequested() ||
        (presentationController.state().requiresNativeWindow() &&
         !output.windowVisible())) {
      requestPlaybackExit(false);
    }
  }

  void emitHeartbeat() {
    if (!perfLog.enabled) return;
    auto nowUi = std::chrono::steady_clock::now();
    if (nowUi - lastUiHeartbeat < kTimingLogHeartbeatInterval) {
      return;
    }
    const bool isPaused = core.playbackState() == PlaybackSessionState::Paused ||
                          audioPlayback.snapshot().paused;
    const bool seeking =
        seekState.seekQueued || core.player().timelineSnapshot().seekPending();
    perfLogAppendf(&perfLog,
                   "video_heartbeat_ui redraw=%d seeker=%d paused=%d",
                   redraw ? 1 : 0, seeking ? 1 : 0, isPaused ? 1 : 0);
    lastUiHeartbeat = nowUi;
  }

  void processInputEvent(
      PlaybackLoopState& loopState, const InputEvent& event,
      playback_video_timeline_preview::PresentationSurface eventSurface) {
    if (loopState == PlaybackLoopState::Stopped) return;
    if (event.type == InputEvent::Type::Resize) {
      // A resize invalidates the geometry under which any desktop-style
      // press was armed. Its eventual button-up must not activate a control
      // at the new layout position.
      seekState.overlayControlPointer.reset();
      overlayControlHover = -1;
      mouseDoubleClickTracker.reset();
    }
    if (event.type != InputEvent::Type::Mouse) {
      mouseDoubleClickTracker.reset();
    }
    if (event.type == InputEvent::Type::Resize) {
      if (eventSurface == playback_video_timeline_preview::
                              PresentationSurface::Terminal) {
        core.markPendingResize();
      }
      redraw = true;
    } else if (event.type == InputEvent::Type::FileDrop &&
               isCommittedFileDropEvent(event.fileDrop)) {
      receiveNativeFileDrop(std::move(event.fileDrop.files));
    } else if (event.type == InputEvent::Type::Key ||
               event.type == InputEvent::Type::Action) {
      playback_session_input::handlePlaybackInputEvent(*this, seekState,
                                                       event);
    } else if (event.type == InputEvent::Type::Mouse) {
      MouseEvent mouse = event.mouse;
      mouseDoubleClickTracker.classifyUsingSystemSettings(
          mouse, screen.cellPixelWidth(), screen.cellPixelHeight());
      playback_session_input::handlePlaybackMouseEvent(*this, seekState,
                                                       mouse);
    } else if (event.type == InputEvent::Type::PointerLeave) {
      mouseDoubleClickTracker.reset();
      playback_session_input::handlePlaybackPointerLeave(*this, seekState);
    }
    if (!loopStopRequested) applyPresenterSync(syncPresentation());
    if (loopStopRequested) {
      loopState = PlaybackLoopState::Stopped;
    }
  }

  void processWindowInputEvents(PlaybackLoopState& loopState) {
    InputEvent event{};
    while (output.pollWindowInput(event)) {
      processInputEvent(
          loopState, event,
          playback_video_timeline_preview::PresentationSurface::VideoWindow);
      if (loopState == PlaybackLoopState::Stopped) break;
    }
  }

  PlaybackControlState buildVideoControlState() const {
    PlaybackControlState state(playbackFileTarget(file), true);
    state.canPlay = true;
    state.canPause = true;
    state.canStop = true;
    state.canPrevious = capabilities.transportHandoff;
    state.canNext = capabilities.transportHandoff;

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
      playback_session_input::sendSeekRequest(
          *this, seekState, seekState.queuedSeekTargetSec);
    }
  }

  void handlePendingResize() {
    core.handlePendingResize(screen, presentationController.state().visual(),
                             redraw);
  }

  struct RefreshState {
    bool nativeWindowActive = false;
    bool presented = false;
    bool debugRefreshDue = false;
  };

  wake_schedule::Deadline computeWakeDeadline(
      const RefreshState& refresh) const {
    const auto now = wake_schedule::Clock::now();
    wake_schedule::Deadline deadline;
    if (const auto osdDeadline = osd.nextDeadline()) {
      wake_schedule::include(deadline, *osdDeadline);
    }
    if (!refresh.nativeWindowActive && config.debugOverlay) {
      wake_schedule::include(
          deadline,
          lastDebugRefresh == wake_schedule::TimePoint::min()
              ? now
              : lastDebugRefresh + kTerminalDebugRefreshInterval);
    }
    if (seekState.seekQueued) {
      wake_schedule::include(
          deadline,
          seekState.lastSeekSentTime == wake_schedule::TimePoint::min()
              ? now
              : seekState.lastSeekSentTime + kSeekThrottleInterval);
    }
    if (!refresh.nativeWindowActive &&
        core.playbackState() == PlaybackSessionState::Active) {
      wake_schedule::include(deadline,
                             now + kTerminalPlaybackRefreshInterval);
    }
    if (perfLog.enabled) {
      wake_schedule::include(deadline,
                             lastUiHeartbeat + kTimingLogHeartbeatInterval);
    }
    return deadline;
  }

  RefreshState refreshState() {
    RefreshState state;
    state.nativeWindowActive = output.windowOpen();
    const PlaybackSessionRefreshResult refresh =
        core.refresh(state.nativeWindowActive, redraw);
    state.presented = refresh.framePresented;
    if (refresh.stateChanged) {
      publishWindowUiState();
      output.requestWindowPresent();
    }
    const auto nowForRefresh = std::chrono::steady_clock::now();
    state.debugRefreshDue =
        !state.nativeWindowActive && config.debugOverlay &&
        (lastDebugRefresh == std::chrono::steady_clock::time_point::min() ||
         nowForRefresh - lastDebugRefresh >= kTerminalDebugRefreshInterval);
    return state;
  }

  bool pump() {
    if (finished) {
      return false;
    }
    if (!initialized) {
      initialized = true;
      if (!initialize()) {
        capturedContinuationState = buildContinuationState();
        finished = true;
        return false;
      }
    }
    if (loopStopRequested) {
      capturedContinuationState = buildContinuationState();
      finished = true;
      return false;
    }

    PlaybackLoopState loopState = PlaybackLoopState::Running;
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
    pollWindowEvents();
    if (loopStopRequested) loopState = PlaybackLoopState::Stopped;

    emitHeartbeat();
    if (loopState == PlaybackLoopState::Running) {
      processWindowInputEvents(loopState);
    }
    if (loopState == PlaybackLoopState::Running) {
      applyPresenterSync(syncPresentation());
      finalizeAudioStart();
      updateWindowCursor();
      flushQueuedSeek();
      handlePendingResize();

      const RefreshState refresh = refreshState();
      if (shouldRenderPlaybackFrame(redraw, refresh.presented,
                                    refresh.debugRefreshDue,
                                    core.playbackState())) {
        renderPlaybackFrame(refresh.presented, loopState);
      } else if (refresh.nativeWindowActive) {
        redraw = false;
        forceRefreshArt = false;
      }
    }

    if (loopState == PlaybackLoopState::Stopped || loopStopRequested ||
        frameOutputState.renderFailed) {
      capturedContinuationState = buildContinuationState();
      finished = true;
      return false;
    }
    return true;
  }

  PlaybackShellTerminalRole terminalRole() const {
    return presentationController.terminalRole();
  }

  std::vector<NativeWaitHandle> activityWaitHandles() const {
    std::vector<NativeWaitHandle> handles;
    handles.reserve(5);
    const auto append = [&](NativeWaitHandle handle) {
      if (handle) handles.push_back(handle);
    };
    append(output.windowOpen() ? core.player().statusChangeWaitHandle()
                               : core.videoFrameWaitHandle());
    append(output.windowInputWaitHandle());
    if (output.windowOpen()) {
      append(output.windowCloseRequestedWaitHandle());
    }
    if (timelinePreviewStarted) {
      append(timelinePreviewProvider.changedWaitHandle());
    }
    append(videoEditWorkspace.waitHandle());
    return handles;
  }

  wake_schedule::Deadline nextWakeDeadline() const {
    if (loopStopRequested || redraw) return wake_schedule::Clock::now();
    RefreshState state;
    state.nativeWindowActive = output.windowOpen();
    return computeWakeDeadline(state);
  }

  PlaybackControlState controlState() const {
    return buildVideoControlState();
  }

  PlaybackPresentationState presentationState() const {
    return presentationController.state();
  }

  bool capturesBrowserInput() const {
    return mediaTaskCancellationPrompt.snapshot().has_value() ||
           videoEditPrompt() != playback_video_edit::Prompt::None;
  }

  void setExternalInputModal(bool modal) {
    if (externalInputModal == modal) return;
    externalInputModal = modal;
    output.window().SetFileDropAcceptanceEnabled(
        nativeFileDropAccepted());
    if (!externalInputModal) resumeDeferredNativeFileDrop();
  }

  bool handleInputEvent(const InputEvent& event) {
    if (finished) return false;
    PlaybackLoopState loopState = PlaybackLoopState::Running;
    processInputEvent(
        loopState, event,
        playback_video_timeline_preview::PresentationSurface::Terminal);
    return true;
  }

  bool handleControlCommand(PlaybackControlCommand command) {
    if (finished) return false;
    playback_session_input::handlePlaybackControlCommand(*this, seekState,
                                                         command);
    if (!loopStopRequested) {
      applyPresenterSync(syncPresentation());
    }
    return true;
  }

  bool seekToRatio(double ratio) {
    if (finished || !std::isfinite(ratio)) return false;
    const int64_t durationUs = core.player().durationUs();
    if (durationUs <= 0) return false;
    const double targetSec =
        std::clamp(ratio, 0.0, 1.0) *
        (static_cast<double>(durationUs) / 1000000.0);
    playback_session_input::queueSeekRequest(*this, seekState, targetSec);
    return true;
  }

  bool toggleWindowPresentation() {
    if (finished) return false;
    const bool handled = dispatch(
        playback_session_input::CommandAction::ToggleWindowPresentation);
    if (handled) applyPresenterSync(syncPresentation());
    return handled;
  }

  bool togglePictureInPicture() {
    if (finished) return false;
    const bool handled = dispatch(
        playback_session_input::CommandAction::TogglePictureInPicture);
    if (handled) applyPresenterSync(syncPresentation());
    return handled;
  }

  bool toggleFullscreen() {
    if (finished) return false;
    const bool handled =
        dispatch(playback_session_input::CommandAction::ToggleFullscreen);
    if (handled) applyPresenterSync(syncPresentation());
    return handled;
  }

  bool activatePresentation() {
    if (finished) return false;
    switch (presentationController.state().primarySurface()) {
      case PlaybackPrimarySurface::Browser:
        activateBrowser();
        return true;
      case PlaybackPrimarySurface::TerminalPlayback:
        activateWindowsConsoleWindow();
        return true;
      case PlaybackPrimarySurface::NativePlayback:
        return output.windowOpen() && output.activateWindow();
    }
    return false;
  }

  bool reloadSubtitles(const std::filesystem::path& preferredTrack) {
    bool selectedPreferred = false;
    subtitleManager.loadForVideo(file);
    hasSubtitles = subtitleManager.selectableTrackCount() > 0;
    selectedPreferred =
        hasSubtitles && subtitleManager.selectTrackForFile(preferredTrack);
    subtitlesEnabled = selectedPreferred || hasSubtitles;
    redraw = true;
    forceRefreshArt = true;
    copiedFrameNeedsRender = true;
    syncOverlayPresentation();
    showEditMessage(selectedPreferred ? "Subtitles ready and enabled"
                                      : (hasSubtitles
                                             ? "Subtitles reloaded"
                                             : "Subtitle file could not be loaded"));
    return hasSubtitles;
  }

  void mediaTaskFinished(
      const playback_media_processing::Completion& completion) {
    const bool promptClosed =
        mediaTaskCancellationPrompt.synchronize(completion);
    if (promptClosed) {
      seekState.overlayControlPointer.reset();
    }
    if (completion.operation ==
            playback_media_processing::Operation::SubtitleGeneration &&
        completion.succeeded()) {
      reloadSubtitles(completion.outputFile);
      if (promptClosed) resumeDeferredNativeFileDrop();
      return;
    }
    syncOverlayPresentation();
    if (promptClosed) resumeDeferredNativeFileDrop();
    showEditMessage(playback_session::mediaTaskFeedback(completion));
  }

  std::optional<playback_session_exit::RequestId> requestHandoff() {
    return requestExternalHandoff();
  }

  std::vector<playback_session::Event> drainEvents() {
    std::vector<playback_session::Event> drained;
    drained.swap(events);
    return drained;
  }

  void requestStop() {
    if (!finished) requestPlaybackExit(false);
  }

  void requestQuit() {
    if (!finished) requestPlaybackExit(true);
  }
};

PlaybackLoopRunner::PlaybackLoopRunner(Args args)
    : impl_(std::make_unique<Impl>(std::move(args))) {}

PlaybackLoopRunner::~PlaybackLoopRunner() = default;

PlaybackLoopRunner::PlaybackLoopRunner(PlaybackLoopRunner&&) noexcept = default;

PlaybackLoopRunner& PlaybackLoopRunner::operator=(
    PlaybackLoopRunner&&) noexcept = default;

bool PlaybackLoopRunner::pump() {
  return impl_->pump();
}

PlaybackShellTerminalRole PlaybackLoopRunner::terminalRole() const {
  return impl_->terminalRole();
}

std::vector<NativeWaitHandle> PlaybackLoopRunner::activityWaitHandles() const {
  return impl_->activityWaitHandles();
}

wake_schedule::Deadline PlaybackLoopRunner::nextWakeDeadline() const {
  return impl_->nextWakeDeadline();
}

PlaybackControlState PlaybackLoopRunner::controlState() const {
  return impl_->controlState();
}

PlaybackPresentationState PlaybackLoopRunner::presentationState() const {
  return impl_->presentationState();
}

bool PlaybackLoopRunner::capturesBrowserInput() const {
  return impl_->capturesBrowserInput();
}

void PlaybackLoopRunner::setExternalInputModal(bool modal) {
  impl_->setExternalInputModal(modal);
}

bool PlaybackLoopRunner::handleInputEvent(const InputEvent& event) {
  return impl_->handleInputEvent(event);
}

bool PlaybackLoopRunner::handleControlCommand(PlaybackControlCommand command) {
  return impl_->handleControlCommand(command);
}

bool PlaybackLoopRunner::seekToRatio(double ratio) {
  return impl_->seekToRatio(ratio);
}

bool PlaybackLoopRunner::toggleWindowPresentation() {
  return impl_->toggleWindowPresentation();
}

bool PlaybackLoopRunner::togglePictureInPicture() {
  return impl_->togglePictureInPicture();
}

bool PlaybackLoopRunner::toggleFullscreen() {
  return impl_->toggleFullscreen();
}

bool PlaybackLoopRunner::activatePresentation() {
  return impl_->activatePresentation();
}

std::optional<playback_session_exit::RequestId>
PlaybackLoopRunner::requestHandoff() {
  return impl_->requestHandoff();
}

bool PlaybackLoopRunner::resolveHandoff(
    playback_session_exit::RequestId requestId, bool accepted) {
  return impl_->resolveHandoff(requestId, accepted);
}

std::vector<playback_session::Event> PlaybackLoopRunner::drainEvents() {
  return impl_->drainEvents();
}

void PlaybackLoopRunner::mediaTaskFinished(
    const playback_media_processing::Completion& completion) {
  impl_->mediaTaskFinished(completion);
}

void PlaybackLoopRunner::requestStop() { impl_->requestStop(); }

void PlaybackLoopRunner::requestQuit() { impl_->requestQuit(); }

void PlaybackLoopRunner::shutdown() { impl_->shutdown(); }

bool PlaybackLoopRunner::quitApplicationRequested() const {
  return impl_->quitApplicationRequested;
}

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
