#include "loop.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
#include "core.h"
#include "core/path_identity.h"
#include "core/runtime_helpers.h"
#include "core/windows_console_window.h"
#include "input.h"
#include "media_task_feedback.h"
#include "mouse_double_click_tracker.h"
#include "output.h"
#include "playback/ascii/frame_output.h"
#include "playback/ascii/screen_renderer.h"
#include "playback/debug/lines.h"
#include "playback/framebuffer/presenter.h"
#include "playback/overlay/overlay.h"
#include "playback/session/background_gpu_admission.h"
#include "playback/session/context_menu_controller.h"
#include "playback/session/media_action_confirmation.h"
#include "playback/session/osd_timeline.h"
#include "playback/session/shutdown_sequence.h"
#include "playback/session/video_edit_workspace.h"
#include "playback/video/chapter/action_catalog.h"
#include "playback/video/chapter/presentation.h"
#include "playback/video/chapter/service.h"
#include "playback/video/chapter/text_evidence.h"
#include "playback/video/edit/overlay_model.h"
#include "playback/video/gpu/gpu_runtime.h"
#include "playback/video/player.h"
#include "playback/video/state/machine.h"
#include "playback/video/subtitle/manager.h"
#include "playback/video/timeline_preview.h"
#include "playback/video/timeline_preview_model.h"
#include "presentation_controller.h"
#include "presentation_model.h"
#include "presentation_projector.h"
#include "state.h"

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
    const VideoPlaybackConfig &config,
    const PlaybackSessionContinuationState &continuityState) {
  if (continuityState.presentation) {
    return *continuityState.presentation;
  }
  return config.enableAscii ? PlaybackPresentationState::terminalAscii()
                            : PlaybackPresentationState::nativeWindowed();
}

} // namespace

struct PlaybackLoopRunner::Impl : playback_session_input::SessionPort {
  enum class ChapterAnalysisTrigger : uint8_t {
    Automatic,
    Manual,
  };

  static constexpr auto kSeekThrottleInterval = std::chrono::milliseconds(50);
  static constexpr auto kFrameCopyMessageDuration =
      std::chrono::milliseconds(1500);
  static constexpr auto kEditMessageDuration = std::chrono::milliseconds(2200);
  static constexpr auto kAnalysisMessageDuration =
      std::chrono::milliseconds(6000);
  static constexpr auto kChapterActivityFrameInterval =
      std::chrono::milliseconds(60);
  static constexpr auto kChapterActivityPeriod =
      std::chrono::milliseconds(1500);
  static constexpr auto kAnimationPreferenceRefreshInterval =
      std::chrono::seconds(1);

  ConsoleScreen &screen;
  AudioPlaybackRuntime &audioPlayback;
  GpuRuntime &gpu;
  const VideoPlaybackConfig config;
  SubtitleManager &subtitleManager;
  playback_video_chapters::Service &chapterAnalysis;
  PerfLog &perfLog;
  const Style &baseStyle;
  const Style &accentStyle;
  const Style &dimStyle;
  const Style &progressEmptyStyle;
  const Style &progressFrameStyle;
  const Color &progressStart;
  const Color &progressEnd;
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
  bool chapterOverviewOpen = false;
  int chapterOverviewScrollOffset = 0;
  bool chapterAutoStartSuppressed = false;
  std::optional<playback_video_chapters::Service::RequestId> chapterRequestId;
  playback_video_chapters::Snapshot chapterSnapshot;
  playback_session::BackgroundGpuAdmissionPolicy chapterGpuAdmission;
  std::chrono::steady_clock::time_point lastChapterGpuHeartbeat =
      std::chrono::steady_clock::time_point::min();
  std::chrono::steady_clock::time_point lastChapterActivityFrame =
      std::chrono::steady_clock::time_point::min();
  std::chrono::steady_clock::time_point lastAnimationPreferenceRefresh =
      std::chrono::steady_clock::time_point::min();
  bool clientAreaAnimationsEnabled = true;
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
  playback_session::MediaActionConfirmationState mediaActionConfirmation;
  std::optional<playback_media_processing::Activity> mediaTaskActivity;
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
  bool shutdownFinished = false;
  std::chrono::steady_clock::time_point lastDebugRefresh =
      std::chrono::steady_clock::time_point::min();
  std::chrono::steady_clock::time_point lastUiHeartbeat =
      std::chrono::steady_clock::now();

  pointer_input::MouseDoubleClickTracker mouseDoubleClickTracker;
  playback_session_input::PlaybackSeekGestureState seekState;
  // The presenter cannot outlive any session-owned state referenced by its
  // published presentation model. The non-owning shutdown sequence is
  // constructed after it and therefore releases its callbacks first.
  PlaybackOutputController output;
  playback_session::ShutdownSequence shutdownSequence;

  explicit Impl(PlaybackLoopRunner::Args args)
      : screen(args.screen), audioPlayback(args.audioPlayback), gpu(args.gpu),
        config(std::move(args.config)), subtitleManager(args.subtitleManager),
        chapterAnalysis(args.chapterAnalysis), perfLog(args.perfLog),
        baseStyle(args.baseStyle), accentStyle(args.accentStyle),
        dimStyle(args.dimStyle), progressEmptyStyle(args.progressEmptyStyle),
        progressFrameStyle(args.progressFrameStyle),
        progressStart(args.progressStart), progressEnd(args.progressEnd),
        timingSink(std::move(args.timingSink)),
        warningSink(std::move(args.warningSink)),
        subtitlesEnabled(args.subtitlesEnabled),
        windowTitle(std::move(args.windowTitle)), file(std::move(args.file)),
        capabilities(args.capabilities),
        mediaProcessingActions(std::move(args.mediaProcessingActions)),
        sessionIntent(args.sessionIntent), enableAudio(args.enableAudio),
        hasSubtitles(args.hasSubtitles),
        presentationController(
            initialPlaybackPresentation(config, args.continuityState),
            args.continuityState.windowPlacement),
        core({args.player, audioPlayback, args.perfLog, args.enableAudio,
              initialPlaybackPresentation(config, args.continuityState)
                  .usesAsciiGrid()}),
        screenResources{gpu,           baseStyle,          accentStyle,
                        dimStyle,      progressEmptyStyle, progressFrameStyle,
                        progressStart, progressEnd,        warningSink,
                        timingSink},
        presentationModel(std::make_shared<playback_session::PresentationModel>(
            playback_session::PresentationModel::Dependencies{
                screenResources})),
        videoEditWorkspace(file, core.player(), timelinePreviewModel,
                           timelinePreviewProvider),
        output(args.player, gpu, windowTitle, presentationModel,
               config.systemMediaCommandOwner),
        shutdownSequence(
            std::vector<playback_session::ShutdownSequence::Participant>{
                {[this]() { timelinePreviewProvider.requestStop(); },
                 [this]() { return timelinePreviewProvider.stopReady(); },
                 [this]() { return timelinePreviewProvider.finishStop(); },
                 [this]() {
                   return timelinePreviewProvider.stopWaitHandles();
                 }},
                {[this]() { videoEditWorkspace.requestStop(); },
                 [this]() { return videoEditWorkspace.stopReady(); },
                 [this]() { return videoEditWorkspace.finishStop(); },
                 [this]() { return videoEditWorkspace.stopWaitHandles(); }},
                {[this]() {
                   perfLogAppendf(&perfLog, "video_shutdown output_stop_begin");
                   perfLogFlush(&perfLog);
                   output.requestCloseWindow();
                 },
                 [this]() { return output.windowCloseReady(); },
                 [this]() { return output.finishCloseWindow(); },
                 [this]() {
                   return std::vector<NativeWaitHandle>{
                       output.windowShutdownWaitHandle()};
                 }},
                {[this]() {
                   perfLogAppendf(&perfLog,
                                  "video_shutdown player_close_begin");
                   perfLogFlush(&perfLog);
                   core.requestPlayerShutdown();
                 },
                 [this]() { return core.playerShutdownReady(); },
                 [this]() { return core.finishPlayerShutdown(); },
                 [this]() {
                   return std::vector<NativeWaitHandle>{
                       core.player().closeWaitHandle()};
                 }}}) {
    core.initialize(screen);
    const playback_video_timeline_preview::Source previewSource{
        file, core.player().videoStreamIndex(), core.player().durationUs(),
        core.player().sourceWidth(), core.player().sourceHeight()};
    timelinePreviewModel.start(previewSource.durationUs,
                               previewSource.sourceWidth,
                               previewSource.sourceHeight);
    timelinePreviewStarted = timelinePreviewProvider.start(previewSource);
    if (!timelinePreviewStarted)
      timelinePreviewModel.stop();
    if (args.subtitleDiscoveryComplete)
      startChapterAnalysis();
    syncOverlayPresentation(false);
    if (sessionIntent == PlaybackSessionIntent::EditVideo) {
      executeVideoEditCommand(playback_video_edit::Command::Open, false);
    }
    applyPresenterSync(syncPresentation());
  }

  ~Impl() {
    if (chapterRequestId)
      chapterAnalysis.cancel(*chapterRequestId);
  }

  void startChapterAnalysis(
      ChapterAnalysisTrigger trigger = ChapterAnalysisTrigger::Automatic) {
    if (trigger == ChapterAnalysisTrigger::Automatic &&
        (!config.enableAutomaticChapterAnalysis ||
         chapterAutoStartSuppressed)) {
      if (!chapterRequestId) {
        chapterSnapshot = {};
        chapterSnapshot.state =
            playback_video_chapters::AnalysisState::Disabled;
        chapterSnapshot.detail =
            !config.enableAutomaticChapterAnalysis
                ? "Automatic chapter analysis is disabled for this launch."
                : "Automatic chapter analysis was cancelled for this video.";
      }
      return;
    }
    if (trigger == ChapterAnalysisTrigger::Manual)
      chapterAutoStartSuppressed = false;
    if (chapterRequestId)
      chapterAnalysis.cancel(*chapterRequestId);
    playback_video_chapters::AnalysisRequest request;
    request.file = file;
    request.videoStreamIndex = core.player().videoStreamIndex();
    request.durationUs = core.player().durationUs();
    request.sourceWidth = core.player().sourceWidth();
    request.sourceHeight = core.player().sourceHeight();
    request.englishText = playback_video_chapters::selectEnglishTextEvidence(
        subtitleManager, file);
    chapterRequestId = chapterAnalysis.start(std::move(request));
    chapterSnapshot = chapterAnalysis.snapshot(*chapterRequestId);
    chapterGpuAdmission.reset();
    lastChapterGpuHeartbeat = std::chrono::steady_clock::time_point::min();
    if (trigger == ChapterAnalysisTrigger::Manual) {
      osd.showMessage("Chapter analysis started in background",
                      playback_session::PlaybackOsdTimeline::Clock::now(),
                      kEditMessageDuration);
    }
    redraw = true;
  }

  void cancelChapterAnalysis() {
    if (!chapterRequestId)
      return;
    chapterAnalysis.cancel(*chapterRequestId);
    chapterRequestId.reset();
    chapterGpuAdmission.reset();
    lastChapterGpuHeartbeat = std::chrono::steady_clock::time_point::min();
  }

  void showEditMessage(const std::string &message) {
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

  bool
  applyExitTransition(const playback_session_exit::Transition &transition) {
    if (!transition.handled)
      return false;
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

  playback_session_exit::Transition
  beginExit(playback_session_exit::Intent intent) {
    if (finished || exitCoordinator.pending())
      return {};

    mediaActionConfirmation.dismiss();

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
                  ? playback_session_exit::
                        Intent{playback_session_exit::QuitApplication{}}
                  : playback_session_exit::Intent{
                        playback_session_exit::StopSession{}});
  }

  bool requestTransportExit(PlaybackTransportCommand command) {
    if (!capabilities.transportHandoff)
      return false;
    return beginExit(playback_session_exit::Transport{command}).handled;
  }

  bool requestOpenFilesExit(const std::vector<std::filesystem::path> &files) {
    if (!capabilities.openFilesHandoff || files.empty())
      return false;
    return beginExit(playback_session_exit::OpenFiles{files}).handled;
  }

  bool nativeFileDropAccepted() const {
    return !externalInputModal && !capturesBrowserInput() &&
           !exitCoordinator.pending();
  }

  bool receiveNativeFileDrop(std::vector<std::filesystem::path> files) {
    if (!capabilities.openFilesHandoff || files.empty())
      return false;
    if (!nativeFileDropAccepted()) {
      deferredNativeFileDrops.push_back(std::move(files));
      return true;
    }
    return requestOpenFilesExit(files);
  }

  void resumeDeferredNativeFileDrop() {
    if (deferredNativeFileDrops.empty() || !nativeFileDropAccepted())
      return;
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
    if (!transition.handled)
      return false;
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
    if (mediaActionConfirmation.snapshot())
      return std::nullopt;
    const playback_session_exit::Transition transition =
        beginExit(playback_session_exit::ExternalHandoff{});
    return transition.handled ? transition.requestId : std::nullopt;
  }

  bool resolveHandoff(playback_session_exit::RequestId requestId,
                      bool accepted) {
    const playback_session_exit::Transition transition =
        exitCoordinator.resolve(requestId, accepted);
    if (!transition.handled)
      return false;
    exitWhenExportSucceeds = false;
    overlayControlHover = -1;
    applyExitTransition(transition);
    if (!accepted) {
      syncOverlayPresentation();
      showEditMessage("Could not complete the requested playback change");
    }
    return true;
  }

  bool abortHandoff(playback_session_exit::RequestId requestId) {
    const playback_session_exit::Transition transition =
        exitCoordinator.abortHandoff(requestId);
    if (!transition.handled)
      return false;
    exitWhenExportSucceeds = false;
    overlayControlHover = -1;
    applyExitTransition(transition);
    syncOverlayPresentation();
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
    if (!result.message.empty())
      showEditMessage(result.message);
  }

  void publishWindowUiState() {
    publishPresentation(buildScreenModel(false, false));
  }

  void syncOverlayPresentation(bool requestPresent = true) {
    playback_media_actions::Context sourceContext =
        mediaProcessingActions.contextForSource(file);
    sourceContext.currentPlayback = true;
    contextMenuController.refresh(
        videoEditWorkspace.edit(), videoEditWorkspace.exportProgress(),
        std::move(sourceContext),
        {chapterSnapshot, chapterRequestId.has_value(), chapterOverviewOpen});
    if (videoEditPrompt() != playback_video_edit::Prompt::None ||
        mediaActionConfirmation.snapshot()) {
      contextMenuController.dismiss();
      timelinePreviewModel.hide(
          playback_video_timeline_preview::PresentationSurface::Terminal);
      timelinePreviewModel.hide(
          playback_video_timeline_preview::PresentationSurface::VideoWindow);
      timelinePreviewProvider.cancelBefore(timelinePreviewModel.requestId());
    }
    output.window().SetFileDropAcceptanceEnabled(nativeFileDropAccepted());
    publishWindowUiState();
    if (!requestPresent)
      return;
    redraw = true;
    output.requestWindowPresent();
  }

  bool openMediaTaskCancellation(
      playback_media_processing::CancellationRequest request) {
    if (videoEditPrompt() != playback_video_edit::Prompt::None ||
        mediaActionConfirmation.snapshot()) {
      return false;
    }
    if (!mediaActionConfirmation.open(std::move(request)))
      return false;
    overlayControlHover = -1;
    syncOverlayPresentation();
    return true;
  }

  bool requestMediaTaskCancellation(playback_media_actions::Action action) {
    std::optional<playback_media_processing::CancellationRequest> request =
        mediaProcessingActions.prepareCancellation(action, file);
    if (!request) {
      syncOverlayPresentation();
      showEditMessage("The background task is no longer running");
      return true;
    }
    return openMediaTaskCancellation(std::move(*request));
  }

  bool requestAudioSeparationSetup() {
    if (videoEditPrompt() != playback_video_edit::Prompt::None ||
        mediaActionConfirmation.snapshot()) {
      return false;
    }
    auto request = mediaProcessingActions.prepareAudioSeparationSetup(
        playback_media_actions::Action::SetUpAudioSeparation, file);
    if (!request) {
      syncOverlayPresentation();
      showEditMessage(
          "Audio separation setup is no longer required or available");
      return true;
    }
    if (!mediaActionConfirmation.open(std::move(*request)))
      return false;
    overlayControlHover = -1;
    syncOverlayPresentation();
    return true;
  }

  bool requestActiveMediaTaskCancellation() {
    if (!mediaTaskActivity || !mediaTaskActivity->cancellable)
      return false;
    std::optional<playback_media_processing::CancellationRequest> current =
        mediaProcessingActions.prepareCancellation(*mediaTaskActivity);
    if (!current) {
      syncOverlayPresentation();
      showEditMessage("The background task is no longer running");
      return true;
    }
    return openMediaTaskCancellation(std::move(*current));
  }

  bool resolveMediaActionConfirmation(
      std::optional<playback_session::MediaActionConfirmationChoice> choice) {
    std::optional<playback_session::MediaActionConfirmationActivation>
        activation = choice ? mediaActionConfirmation.resolve(*choice)
                            : mediaActionConfirmation.activate();
    if (!activation)
      return false;
    overlayControlHover = -1;
    seekState.overlayControlPointer.reset();
    if (const std::optional<playback_media_processing::ActionResult> result =
            playback_session::executeConfirmedMediaAction(
                mediaProcessingActions, *activation)) {
      showEditMessage(result->feedback);
    }
    syncOverlayPresentation();
    resumeDeferredNativeFileDrop();
    return true;
  }

  bool moveMediaActionConfirmationSelection(int direction) {
    if (!mediaActionConfirmation.moveSelection(direction))
      return false;
    overlayControlHover = -1;
    syncOverlayPresentation();
    return true;
  }

  void pollVideoEditExport() {
    const playback_session::VideoEditPollResult result =
        videoEditWorkspace.poll();
    if (!result.changed)
      return;
    overlayControlHover = -1;
    syncOverlayPresentation();
    if (!result.message.empty())
      showEditMessage(result.message);
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
    if (!seekState.pendingVideoEditBoundaryCommit)
      return;
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
    if (action == playback_media_actions::Action::SetUpAudioSeparation) {
      return requestAudioSeparationSetup();
    }
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
    case playback_media_actions::Action::SetUpAudioSeparation:
    case playback_media_actions::Action::CancelAudioSeparationSetup:
    case playback_media_actions::Action::SeparateAudio:
    case playback_media_actions::Action::CancelAudioSeparation:
      return true;
    }
    return false;
  }

  bool executeChapterAction(playback_video_chapters::Action action) {
    using Action = playback_video_chapters::Action;
    switch (action) {
    case Action::StartAnalysis:
      startChapterAnalysis(ChapterAnalysisTrigger::Manual);
      chapterOverviewOpen = false;
      chapterOverviewScrollOffset = 0;
      syncOverlayPresentation();
      return true;
    case Action::CancelAnalysis:
      if (!chapterRequestId)
        return false;
      cancelChapterAnalysis();
      chapterAutoStartSuppressed = true;
      chapterSnapshot = {};
      chapterSnapshot.state = playback_video_chapters::AnalysisState::Disabled;
      chapterSnapshot.detail = "Chapter analysis was cancelled for this video.";
      chapterOverviewOpen = false;
      chapterOverviewScrollOffset = 0;
      syncOverlayPresentation();
      showEditMessage("Chapter analysis cancelled");
      return true;
    case Action::InstallModels:
      if (!chapterRequestId ||
          !chapterAnalysis.requestInstallation(*chapterRequestId)) {
        return false;
      }
      showEditMessage("Installing chapter models in background");
      return true;
    case Action::CancelInstallation:
      if (!chapterRequestId ||
          !chapterAnalysis.cancelInstallation(*chapterRequestId)) {
        return false;
      }
      showEditMessage("Cancelling chapter model installation");
      return true;
    case Action::TogglePanel:
      if (!chapterSnapshot.ready())
        return false;
      chapterOverviewOpen = !chapterOverviewOpen;
      chapterOverviewScrollOffset = 0;
      syncOverlayPresentation();
      return true;
    case Action::ShowStatus: {
      std::string message =
          playback_video_chapters::analysisStateLabel(chapterSnapshot.state);
      if (!chapterSnapshot.detail.empty()) {
        message += ": " + chapterSnapshot.detail;
      } else if (!chapterSnapshot.phase.empty()) {
        message += ": " + chapterSnapshot.phase;
      }
      osd.showMessage(std::move(message),
                      playback_session::PlaybackOsdTimeline::Clock::now(),
                      kAnalysisMessageDuration);
      redraw = true;
      publishWindowUiState();
      output.requestWindowPresent();
      return true;
    }
    }
    return false;
  }

  bool executeContextMenuCommand(
      const playback_session::ContextMenuCommand &command) {
    if (const auto *media =
            std::get_if<playback_media_actions::Action>(&command)) {
      return executeMediaAction(*media);
    }
    if (const auto *edit =
            std::get_if<playback_video_edit::Command>(&command)) {
      return executeVideoEditCommand(*edit);
    }
    return executeChapterAction(
        std::get<playback_video_chapters::Action>(command));
  }

  bool waitForVideoEditExportAndExit() {
    if (!exitCoordinator.confirmationVisible())
      return false;
    const playback_video_edit::ExitExportAction action =
        playback_video_edit::exitExportAction(videoEditWorkspace.exitContext());
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

  bool
  handleContextMenuInput(const playback_session::ContextMenuInput &request) {
    bool handled = false;
    std::optional<playback_session::ContextMenuCommand> activatedCommand;
    using InputKind = playback_session::ContextMenuInputKind;
    switch (request.kind) {
    case InputKind::Open: {
      if (videoEditPrompt() != playback_video_edit::Prompt::None ||
          mediaActionConfirmation.snapshot()) {
        return false;
      }
      if (videoEditWorkspace.active()) {
        if (request.timelineUs) {
          videoEditWorkspace.selectCutAt(*request.timelineUs,
                                         request.timelineToleranceUs);
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
      handled = contextMenuController.open(request.surface, xRatio, yRatio);
      if (handled) {
        const auto previewSurface =
            request.surface == playback_session::ContextMenuSurface::Terminal
                ? playback_video_timeline_preview::PresentationSurface::Terminal
                : playback_video_timeline_preview::PresentationSurface::
                      VideoWindow;
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
        [this](auto value) { return executeInputCommand(std::move(value)); },
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
    state.mediaActionConfirmationPrompt =
        mediaActionConfirmation.snapshot().has_value();
    state.contextMenuVisible = contextMenuController.visible();
    state.chapterOverviewOpen = chapterOverviewOpen;
    state.playbackControlsVisible = osd.controlsVisible();
    state.stopRequested = loopStopRequested;
    return state;
  }

  playback_overlay::InteractionHit
  hitTest(const playback_session_input::InteractionRequest &request)
      const override {
    if (request.surface ==
        playback_video_timeline_preview::PresentationSurface::VideoWindow) {
      return output.window().OverlayHitAt(request.x, request.y,
                                          request.capturedProgress);
    }
    if (request.scaleX != 1.0 || request.scaleY != 1.0) {
      return playback_overlay::interactionHitAtTransformed(
          frameOutputState.overlayInteractions, 0.0, 0.0, request.scaleX,
          request.scaleY, request.x, request.y, request.capturedProgress);
    }
    return playback_overlay::interactionHitAt(
        frameOutputState.overlayInteractions, request.x, request.y,
        request.capturedProgress);
  }

  bool toggleSubtitles() {
    if (!hasSubtitles)
      return false;
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
      if (!core.snapshot().audioAvailable)
        return false;
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
    case Action::ToggleChapterOverview:
      return executeChapterAction(
          chapterSnapshot.ready()
              ? playback_video_chapters::Action::TogglePanel
              : (chapterSnapshot.state ==
                         playback_video_chapters::AnalysisState::Disabled
                     ? playback_video_chapters::Action::StartAnalysis
                     : playback_video_chapters::Action::ShowStatus));
    case Action::CloseChapterOverview:
      if (!chapterOverviewOpen)
        return false;
      chapterOverviewOpen = false;
      chapterOverviewScrollOffset = 0;
      syncOverlayPresentation();
      return true;
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
        osd.showMessage("Frame copied to clipboard",
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
    case Action::CancelActiveMediaTask:
      return requestActiveMediaTaskCancellation();
    case Action::ConfirmMediaAction:
      return resolveMediaActionConfirmation(
          playback_session::MediaActionConfirmationChoice::Primary);
    case Action::DismissMediaAction:
      return resolveMediaActionConfirmation(
          playback_session::MediaActionConfirmationChoice::Secondary);
    case Action::ActivateSelectedMediaActionConfirmation:
      return resolveMediaActionConfirmation(std::nullopt);
    case Action::SelectPreviousMediaActionConfirmation:
      return moveMediaActionConfirmationSelection(-1);
    case Action::SelectNextMediaActionConfirmation:
      return moveMediaActionConfirmationSelection(1);
    }
    return false;
  }

  bool executeInputCommand(playback_session_input::TransportRequest request) {
    return requestTransportExit(request.command);
  }

  bool executeInputCommand(
      playback_session_input::ChapterNavigationRequest request) {
    const playback_session_input::TransportSnapshot transport = core.snapshot();
    const playback_video_chapters::Snapshot chapters =
        chapterPresentationSnapshot(videoEditWorkspace.edit());
    const playback_video_chapters::Chapter *current =
        playback_video_chapters::chapterAt(chapters, transport.positionUs);
    if (!current)
      return false;
    auto chapter =
        std::find_if(chapters.chapters.begin(), chapters.chapters.end(),
                     [&](const playback_video_chapters::Chapter &candidate) {
                       return candidate.id == current->id;
                     });
    if (chapter == chapters.chapters.end())
      return false;
    const std::optional<std::int64_t> target =
        playback_video_chapters::navigationTarget(
            chapters, transport.positionUs, request.direction);
    return target &&
           executeInputCommand(playback_session_input::SeekTo{*target});
  }

  bool executeInputCommand(playback_session_input::SeekToChapter request) {
    const playback_video_chapters::Snapshot chapters =
        chapterPresentationSnapshot(videoEditWorkspace.edit());
    const auto chapter =
        std::find_if(chapters.chapters.begin(), chapters.chapters.end(),
                     [&](const playback_video_chapters::Chapter &candidate) {
                       return candidate.startUs == request.timelineStartUs;
                     });
    if (chapter == chapters.chapters.end())
      return false;
    return executeInputCommand(
        playback_session_input::SeekTo{chapter->startUs});
  }

  bool executeInputCommand(
      playback_session_input::SetChapterOverviewScroll request) {
    if (!chapterOverviewOpen || !chapterSnapshot.ready())
      return false;
    const int next = std::max(0, request.offset);
    if (next == chapterOverviewScrollOffset)
      return true;
    chapterOverviewScrollOffset = next;
    redraw = true;
    forceRefreshArt = true;
    publishWindowUiState();
    output.requestWindowPresent();
    return true;
  }

  bool executeInputCommand(playback_session_input::VideoEditRequest request) {
    return executeVideoEditCommand(request.command);
  }

  bool executeInputCommand(playback_session_input::ContextMenuRequest request) {
    return handleContextMenuInput(request.input);
  }

  bool
  executeInputCommand(playback_session_input::MoveVideoEditBoundary request) {
    if (exitCoordinator.pending() ||
        !videoEditWorkspace.moveBoundary(request.boundary,
                                         request.timelineUs)) {
      return false;
    }
    syncOverlayPresentation();
    return true;
  }

  bool
  executeInputCommand(playback_session_input::PlaybackExitRequest request) {
    requestPlaybackExit(request.quitApplication);
    return true;
  }

  bool
  executeInputCommand(playback_session_input::TimelinePreviewRequest request) {
    auto update = timelinePreviewModel.hover(request.surface, request.ratio,
                                             request.progressUnits);
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

  bool
  executeInputCommand(playback_session_input::ClearTimelinePreview request) {
    if (!timelinePreviewModel.hide(request.surface))
      return false;
    timelinePreviewProvider.cancelBefore(timelinePreviewModel.requestId());
    redraw = true;
    publishWindowUiState();
    output.requestWindowPresent();
    return true;
  }

  bool
  executeInputCommand(playback_session_input::ShowPlaybackControls request) {
    osd.showControls(playback_session::PlaybackOsdTimeline::Clock::now(),
                     request.duration);
    redraw = true;
    return true;
  }

  bool
  executeInputCommand(playback_session_input::SetOverlayControlHover request) {
    if (overlayControlHover == request.token)
      return false;
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
    if (!core.seekTo(request.targetUs))
      return false;
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
    if (!core.seekBy(request.deltaUs))
      return false;
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
    const playback_video_edit::EditSnapshot edit = videoEditWorkspace.edit();
    const playback_video_edit::ExportProgress editExport =
        videoEditWorkspace.exportProgress();
    return config.debugOverlay || osd.controlsVisible() ||
           playback_video_edit::needsOverlayPresentation(edit, editExport,
                                                         videoEditPrompt()) ||
           contextMenuController.visible();
  }

  bool chapterActivityControlVisible() const {
    return chapterSnapshot.running() && overlayVisible() &&
           !contextMenuController.visible() &&
           !mediaActionConfirmation.snapshot() &&
           videoEditPrompt() == playback_video_edit::Prompt::None &&
           !videoEditWorkspace.edit().active;
  }

  bool chapterActivityMotionVisible() const {
    return chapterActivityControlVisible() && clientAreaAnimationsEnabled;
  }

  void refreshAnimationPreference() {
    if (!chapterSnapshot.running())
      return;
    const auto now = std::chrono::steady_clock::now();
    if (lastAnimationPreferenceRefresh !=
            std::chrono::steady_clock::time_point::min() &&
        now - lastAnimationPreferenceRefresh <
            kAnimationPreferenceRefreshInterval) {
      return;
    }
    BOOL enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) {
      const bool nextEnabled = enabled != FALSE;
      if (nextEnabled != clientAreaAnimationsEnabled) {
        clientAreaAnimationsEnabled = nextEnabled;
        redraw = true;
      }
    }
    lastAnimationPreferenceRefresh = now;
  }

  double chapterActivityPhase() const {
    if (!chapterActivityMotionVisible())
      return 0.0;
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    const auto period =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            kChapterActivityPeriod);
    const auto withinPeriod = elapsed % period;
    return std::chrono::duration<double>(withinPeriod).count() /
           std::chrono::duration<double>(period).count();
  }

  void updateChapterActivityAnimation() {
    if (!chapterActivityMotionVisible()) {
      lastChapterActivityFrame = std::chrono::steady_clock::time_point::min();
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (lastChapterActivityFrame !=
            std::chrono::steady_clock::time_point::min() &&
        now - lastChapterActivityFrame < kChapterActivityFrameInterval) {
      return;
    }
    lastChapterActivityFrame = now;
    redraw = true;
  }

  playback_overlay::PlaybackOsdSnapshot osdSnapshot() const {
    playback_overlay::PlaybackOsdSnapshot snapshot = osd.snapshot();
    snapshot.controlsVisible = snapshot.controlsVisible || config.debugOverlay;
    return snapshot;
  }

  PlaybackPresentationSyncResult syncPresentation() {
    return presentationController.synchronize(output);
  }

  void applyPresenterSync(const PlaybackPresentationSyncResult &syncResult) {
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
      core.setAsciiPresentation(screen,
                                syncResult.appliedState.usesAsciiGrid());
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

  void beginShutdown() {
    if (shutdownSequence.requested())
      return;
    perfLogAppendf(&perfLog, "video_shutdown begin");
    perfLogFlush(&perfLog);
    timelinePreviewModel.stop();
    timelinePreviewStarted = false;
    cancelChapterAnalysis();
    (void)shutdownSequence.requestStop();
  }

  bool shutdownReady() { return shutdownSequence.ready(); }

  bool finishShutdown() {
    if (shutdownFinished)
      return true;
    if (!shutdownSequence.finish())
      return false;
    perfLogAppendf(&perfLog, "video_shutdown output_stop_end");
    perfLogFlush(&perfLog);
    perfLogAppendf(&perfLog, "video_shutdown player_close_end");
    perfLogFlush(&perfLog);
    core.finishAudioShutdown();
    perfLogAppendf(&perfLog, "video_shutdown audio_released");
    perfLogFlush(&perfLog);
    shutdownFinished = true;
    return true;
  }

  std::vector<NativeWaitHandle> shutdownWaitHandles() const {
    return shutdownSequence.waitHandles();
  }

  void shutdown() {
    if (shutdownFinished)
      return;
    beginShutdown();
    timelinePreviewProvider.stop();
    videoEditWorkspace.stop();
    output.closeWindow();
    core.shutdownPlayer();
    (void)shutdownSequence.finish();
    core.finishAudioShutdown();
    shutdownFinished = true;
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

  void refreshChapterSnapshot() {
    if (!chapterRequestId)
      return;
    const playback_video_chapters::Snapshot next =
        chapterAnalysis.snapshot(*chapterRequestId);
    if (next.revision == chapterSnapshot.revision)
      return;
    const bool logMilestone = next.state != chapterSnapshot.state ||
                              next.phase != chapterSnapshot.phase ||
                              next.detail != chapterSnapshot.detail ||
                              next.warning != chapterSnapshot.warning;
    const bool replacedReadyDocument =
        next.state == playback_video_chapters::AnalysisState::Ready &&
        (chapterSnapshot.state !=
             playback_video_chapters::AnalysisState::Ready ||
         next.revision != chapterSnapshot.revision);
    chapterSnapshot = next;
    if (perfLog.enabled && logMilestone) {
      perfLogAppendf(
          &perfLog,
          "chapter_analysis_state revision=%llu state=%s progress=%.3f "
          "phase=%s detail=%s warning=%s",
          static_cast<unsigned long long>(chapterSnapshot.revision),
          playback_video_chapters::analysisStateLabel(chapterSnapshot.state),
          chapterSnapshot.progress.value_or(-1.0),
          chapterSnapshot.phase.c_str(), chapterSnapshot.detail.c_str(),
          chapterSnapshot.warning.c_str());
    }
    if (!chapterSnapshot.ready()) {
      chapterOverviewOpen = false;
      chapterOverviewScrollOffset = 0;
    } else if (replacedReadyDocument) {
      chapterOverviewScrollOffset = 0;
    }
    if (chapterSnapshot.state ==
            playback_video_chapters::AnalysisState::Unsupported ||
        chapterSnapshot.state ==
            playback_video_chapters::AnalysisState::Failed) {
      std::string message =
          playback_video_chapters::analysisStateLabel(chapterSnapshot.state);
      if (!chapterSnapshot.detail.empty()) {
        message += ": " + chapterSnapshot.detail;
      }
      osd.showMessage(std::move(message),
                      playback_session::PlaybackOsdTimeline::Clock::now(),
                      kAnalysisMessageDuration);
    } else if (replacedReadyDocument && !chapterSnapshot.warning.empty()) {
      osd.showMessage(chapterSnapshot.warning,
                      playback_session::PlaybackOsdTimeline::Clock::now(),
                      kAnalysisMessageDuration);
    }
    redraw = true;
    forceRefreshArt = true;
    syncOverlayPresentation();
  }

  void updateChapterGpuPriority() {
    if (!chapterRequestId)
      return;
    if (!chapterSnapshot.running() &&
        chapterSnapshot.state !=
            playback_video_chapters::AnalysisState::CheckingSupport) {
      const bool wasAllowed = chapterGpuAdmission.allowed();
      chapterGpuAdmission.reset();
      if (wasAllowed) {
        chapterAnalysis.setBackgroundGpuAllowed(*chapterRequestId, false);
      }
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    const PlaybackSessionState playbackState = core.playbackState();
    playback_session::BackgroundGpuSignals signals;
    if (playbackState == PlaybackSessionState::Paused ||
        playbackState == PlaybackSessionState::Ended) {
      signals.foreground =
          playback_session::ForegroundPlaybackActivity::Inactive;
    } else if (playbackState == PlaybackSessionState::Active) {
      signals.foreground = playback_session::ForegroundPlaybackActivity::Active;
    }

    PlayerDebugInfo debug;
    if (playbackState == PlaybackSessionState::Active) {
      debug = core.player().debugInfo();
      signals.seekPending = core.player().seekPending();
      signals.buffering = core.player().isBuffering();
      signals.audioStarved = debug.audioStarved;
      signals.hasVideoFrame = debug.hasVideoFrame;
      signals.videoQueueDepth = debug.videoQueueDepth;
      signals.lastPresentedDurationUs = debug.lastPresentedDurationUs;
    }
    const playback_session::BackgroundGpuDecision decision =
        chapterGpuAdmission.update(signals, now);

    if (playbackState == PlaybackSessionState::Active) {
      if (perfLog.enabled &&
          (lastChapterGpuHeartbeat ==
               std::chrono::steady_clock::time_point::min() ||
           now - lastChapterGpuHeartbeat >= kTimingLogHeartbeatInterval)) {
        const auto healthyMs =
            decision.healthyFor.value_or(std::chrono::milliseconds{-1});
        perfLogAppendf(
            &perfLog,
            "chapter_gpu_scheduler allowed=%d changed=%d stable=%d seek=%d "
            "buffering=%d audio_starved=%d headroom=%d qv=%zu frame_us=%lld "
            "healthy_ms=%lld",
            decision.allowed ? 1 : 0, decision.changed ? 1 : 0,
            decision.foregroundStable ? 1 : 0, signals.seekPending ? 1 : 0,
            signals.buffering ? 1 : 0, signals.audioStarved ? 1 : 0,
            decision.queueHasHeadroom ? 1 : 0, debug.videoQueueDepth,
            static_cast<long long>(debug.lastPresentedDurationUs),
            static_cast<long long>(healthyMs.count()));
        lastChapterGpuHeartbeat = now;
      }
    }
    if (decision.changed) {
      chapterAnalysis.setBackgroundGpuAllowed(*chapterRequestId,
                                              decision.allowed);
    }
  }

  playback_video_chapters::Snapshot chapterPresentationSnapshot(
      const playback_video_edit::EditSnapshot &edit) const {
    if (!chapterSnapshot.ready() || !edit.hasEdits)
      return chapterSnapshot;
    if (edit.sourceDurationUs != chapterSnapshot.durationUs ||
        edit.timelineDurationUs <= 0 || edit.clips.empty()) {
      return {};
    }
    std::vector<playback_video_chapters::MarkerTimelineSegment> segments;
    segments.reserve(edit.clips.size());
    for (const playback_video_edit::EditClipSnapshot &clip : edit.clips) {
      segments.push_back(
          {clip.source.startUs, clip.source.endUs, clip.timelineStartUs});
    }
    const std::optional<playback_video_chapters::Snapshot> projected =
        playback_video_chapters::projectToPresentationTimeline(
            chapterSnapshot, edit.timelineDurationUs, segments);
    return projected.value_or(playback_video_chapters::Snapshot{});
  }

  playback_video_timeline_preview::Snapshot timelinePreviewSnapshot(
      playback_video_timeline_preview::PresentationSurface surface) const {
    playback_video_timeline_preview::Snapshot snapshot =
        timelinePreviewModel.snapshotFor(surface);
    if (snapshot.hoverActive) {
      const playback_video_chapters::Snapshot chapters =
          chapterPresentationSnapshot(videoEditWorkspace.edit());
      snapshot.metadataLines =
          playback_video_chapters::previewMetadata(chapters, snapshot.targetUs);
    }
    return snapshot;
  }

  playback_screen_renderer::PlaybackMediaPresentation
  captureMediaPresentation() const {
    const Player &player = core.player();
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
      const playback_screen_renderer::PlaybackMediaPresentation &media,
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
    projection.subtitleRenderError = output.window().GetSubtitleRenderError();
    projection.contextMenu = contextMenuController.snapshotFor(
        surface ==
                playback_video_timeline_preview::PresentationSurface::Terminal
            ? playback_session::ContextMenuSurface::Terminal
            : playback_session::ContextMenuSurface::VideoWindow);
    projection.videoEdit = videoEditWorkspace.edit();
    projection.videoEditExport = videoEditWorkspace.exportProgress();
    projection.videoEditPrompt = videoEditPrompt();
    projection.mediaActionConfirmationPrompt =
        mediaActionConfirmation.snapshot();
    projection.mediaTaskActivity = mediaTaskActivity;
    projection.chapters = chapterPresentationSnapshot(projection.videoEdit);
    projection.chapterControlVisible = true;
    projection.chapterActivityPhase = chapterActivityPhase();
    projection.chapterActivityMotionEnabled = clientAreaAnimationsEnabled;
    projection.chapterOverviewOpen = chapterOverviewOpen;
    projection.chapterOverviewScrollOffset = chapterOverviewScrollOffset;
    if (config.debugOverlay &&
        surface ==
            playback_video_timeline_preview::PresentationSurface::VideoWindow) {
      projection.debugLines.push_back(output.window().OutputColorDebugLine());
      projection.debugLines.push_back(
          playback_debug_lines::videoFrameDebugLine(media.debug));
    }
    return playback_session::projectPlaybackOverlay(std::move(projection));
  }

  playback_screen_renderer::PlaybackScreenModel
  buildScreenModel(bool clearHistory, bool frameChanged) {
    playback_screen_renderer::PlaybackScreenModel model;
    model.debugOverlay = config.debugOverlay;
    model.visualMode = presentationController.state().visual();
    model.enableAudio = enableAudio;
    model.nativeWindowActive = output.windowOpen();
    model.allowAsciiCpuFallback = false;
    model.media = captureMediaPresentation();
    model.timelinePreview = timelinePreviewSnapshot(
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
    if (!overlayVisible())
      overlayControlHover = -1;
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
      const playback_screen_renderer::PlaybackScreenModel &model) {
    playback_session::PresentationModel::Revision revision;
    revision.textGrid = model;
    revision.textGrid.timelinePreview = timelinePreviewSnapshot(
        playback_video_timeline_preview::PresentationSurface::VideoWindow);
    revision.textGrid.overlay = buildOverlayState(
        model.media, model.playbackState,
        playback_video_timeline_preview::PresentationSurface::VideoWindow);
    revision.window = playback_session::projectWindowUiState(revision.textGrid);
    presentationModel->publish(std::move(revision));
  }

  void
  renderTerminal(const playback_screen_renderer::PlaybackScreenModel &model) {
    playback_screen_renderer::PlaybackScreenTarget target{
        screen,
        output.frameCache(),
        art,
        timelinePreviewArt,
        core.presentationFrame(),
        frameOutputState};
    playback_screen_renderer::renderPlaybackScreen(screenResources, target,
                                                   model);
  }

  void renderPlaybackFrame(bool presented, PlaybackLoopState &loopState) {
    if (presentationController.terminalRole() ==
        PlaybackShellTerminalRole::Browser) {
      publishPresentation(buildScreenModel(false, presented));
      if (chapterActivityControlVisible() && output.windowOpen())
        output.requestWindowPresent();
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
    if (chapterActivityControlVisible() && output.windowOpen())
      output.requestWindowPresent();
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
        (output.windowLifecycle() == PlaybackWindowLifecycle::Open &&
         presentationController.state().requiresNativeWindow() &&
         !output.windowVisible())) {
      requestPlaybackExit(false);
    }
  }

  void emitHeartbeat() {
    if (!perfLog.enabled)
      return;
    auto nowUi = std::chrono::steady_clock::now();
    if (nowUi - lastUiHeartbeat < kTimingLogHeartbeatInterval) {
      return;
    }
    const bool isPaused =
        core.playbackState() == PlaybackSessionState::Paused ||
        audioPlayback.snapshot().paused;
    const bool seeking =
        seekState.seekQueued || core.player().timelineSnapshot().seekPending();
    perfLogAppendf(&perfLog, "video_heartbeat_ui redraw=%d seeker=%d paused=%d",
                   redraw ? 1 : 0, seeking ? 1 : 0, isPaused ? 1 : 0);
    lastUiHeartbeat = nowUi;
  }

  void processInputEvent(
      PlaybackLoopState &loopState, const InputEvent &event,
      playback_video_timeline_preview::PresentationSurface eventSurface) {
    if (loopState == PlaybackLoopState::Stopped)
      return;
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
      if (eventSurface ==
          playback_video_timeline_preview::PresentationSurface::Terminal) {
        core.markPendingResize();
      }
      redraw = true;
    } else if (event.type == InputEvent::Type::FileDrop &&
               isCommittedFileDropEvent(event.fileDrop)) {
      receiveNativeFileDrop(std::move(event.fileDrop.files));
    } else if (event.type == InputEvent::Type::Key ||
               event.type == InputEvent::Type::Action) {
      playback_session_input::handlePlaybackInputEvent(*this, seekState, event);
    } else if (event.type == InputEvent::Type::Mouse) {
      MouseEvent mouse = event.mouse;
      mouseDoubleClickTracker.classifyUsingSystemSettings(
          mouse, screen.cellPixelWidth(), screen.cellPixelHeight());
      playback_session_input::handlePlaybackMouseEvent(*this, seekState, mouse);
    } else if (event.type == InputEvent::Type::PointerLeave) {
      mouseDoubleClickTracker.reset();
      playback_session_input::handlePlaybackPointerLeave(*this, seekState);
    }
    if (!loopStopRequested)
      applyPresenterSync(syncPresentation());
    if (loopStopRequested) {
      loopState = PlaybackLoopState::Stopped;
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
      state.positionSec = static_cast<double>(timeline.positionUs) / 1000000.0;
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
    bool canSend = (seekState.lastSeekSentTime ==
                    std::chrono::steady_clock::time_point::min()) ||
                   (now - seekState.lastSeekSentTime >= kSeekThrottleInterval);
    if (canSend) {
      playback_session_input::sendSeekRequest(*this, seekState,
                                              seekState.queuedSeekTargetSec);
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

  wake_schedule::Deadline
  computeWakeDeadline(const RefreshState &refresh) const {
    const auto now = wake_schedule::Clock::now();
    wake_schedule::Deadline deadline;
    if (const auto osdDeadline = osd.nextDeadline()) {
      wake_schedule::include(deadline, *osdDeadline);
    }
    if (!refresh.nativeWindowActive && config.debugOverlay) {
      wake_schedule::include(
          deadline, lastDebugRefresh == wake_schedule::TimePoint::min()
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
      wake_schedule::include(deadline, now + kTerminalPlaybackRefreshInterval);
    }
    if (chapterActivityMotionVisible()) {
      wake_schedule::include(
          deadline,
          lastChapterActivityFrame == wake_schedule::TimePoint::min()
              ? now
              : lastChapterActivityFrame + kChapterActivityFrameInterval);
    }
    if (chapterActivityControlVisible()) {
      wake_schedule::include(
          deadline,
          lastAnimationPreferenceRefresh == wake_schedule::TimePoint::min()
              ? now
              : lastAnimationPreferenceRefresh +
                    kAnimationPreferenceRefreshInterval);
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
    if (chapterRequestId && chapterAnalysis.consumeChanged()) {
      refreshChapterSnapshot();
    }
    refreshAnimationPreference();
    updateChapterActivityAnimation();
    updateChapterGpuPriority();
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
    if (loopStopRequested)
      loopState = PlaybackLoopState::Stopped;

    emitHeartbeat();
    if (loopState == PlaybackLoopState::Running) {
      applyPresenterSync(syncPresentation());
      pollWindowEvents();
      if (loopStopRequested)
        loopState = PlaybackLoopState::Stopped;
    }
    if (loopState == PlaybackLoopState::Running) {
      finalizeAudioStart();
      updateWindowCursor();
      flushQueuedSeek();
      handlePendingResize();

      const RefreshState refresh = refreshState();
      updateChapterGpuPriority();
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
    handles.reserve(8);
    const auto append = [&](NativeWaitHandle handle) {
      if (handle)
        handles.push_back(handle);
    };
    append(output.windowOpen() ? core.player().statusChangeWaitHandle()
                               : core.videoFrameWaitHandle());
    if (output.windowOpen()) {
      append(output.windowInputWaitHandle());
      append(output.windowCloseRequestedWaitHandle());
    }
    append(output.windowTransitionWaitHandle());
    if (timelinePreviewStarted) {
      append(timelinePreviewProvider.changedWaitHandle());
    }
    if (chapterRequestId) {
      append(chapterAnalysis.changedWaitHandle());
    }
    for (const NativeWaitHandle handle :
         videoEditWorkspace.activityWaitHandles()) {
      append(handle);
    }
    return handles;
  }

  wake_schedule::Deadline nextWakeDeadline() const {
    if (loopStopRequested || redraw)
      return wake_schedule::Clock::now();
    RefreshState state;
    state.nativeWindowActive = output.windowOpen();
    wake_schedule::Deadline deadline = computeWakeDeadline(state);
    if (chapterRequestId) {
      if (const auto admissionDeadline =
              chapterGpuAdmission.nextEvaluationDeadline()) {
        wake_schedule::include(deadline, *admissionDeadline);
      }
    }
    return deadline;
  }

  playback_session::ViewSnapshot viewSnapshot() const {
    return {buildVideoControlState(), presentationController.state()};
  }

  bool capturesBrowserInput() const {
    return mediaActionConfirmation.snapshot().has_value() ||
           videoEditPrompt() != playback_video_edit::Prompt::None;
  }

  void setExternalInputModal(bool modal) {
    if (externalInputModal == modal)
      return;
    externalInputModal = modal;
    output.window().SetFileDropAcceptanceEnabled(nativeFileDropAccepted());
    if (!externalInputModal)
      resumeDeferredNativeFileDrop();
  }

  bool handleInputEvent(const InputEvent &event) {
    if (finished)
      return false;
    PlaybackLoopState loopState = PlaybackLoopState::Running;
    processInputEvent(
        loopState, event,
        playback_video_timeline_preview::PresentationSurface::Terminal);
    return true;
  }

  bool pollWindowInput(InputEvent &event) {
    return !finished && output.pollWindowInput(event);
  }

  bool handleWindowInputEvent(const InputEvent &event) {
    if (finished)
      return false;
    PlaybackLoopState loopState = PlaybackLoopState::Running;
    processInputEvent(
        loopState, event,
        playback_video_timeline_preview::PresentationSurface::VideoWindow);
    return true;
  }

  bool handleControlCommand(PlaybackControlCommand command) {
    if (finished)
      return false;
    playback_session_input::handlePlaybackControlCommand(*this, seekState,
                                                         command);
    if (!loopStopRequested) {
      applyPresenterSync(syncPresentation());
    }
    return true;
  }

  bool seekToRatio(double ratio) {
    if (finished || !std::isfinite(ratio))
      return false;
    const int64_t durationUs = core.player().durationUs();
    if (durationUs <= 0)
      return false;
    const double targetSec = std::clamp(ratio, 0.0, 1.0) *
                             (static_cast<double>(durationUs) / 1000000.0);
    playback_session_input::queueSeekRequest(*this, seekState, targetSec);
    return true;
  }

  bool toggleWindowPresentation() {
    if (finished)
      return false;
    const bool handled = dispatch(
        playback_session_input::CommandAction::ToggleWindowPresentation);
    if (handled)
      applyPresenterSync(syncPresentation());
    return handled;
  }

  bool togglePictureInPicture() {
    if (finished)
      return false;
    const bool handled =
        dispatch(playback_session_input::CommandAction::TogglePictureInPicture);
    if (handled)
      applyPresenterSync(syncPresentation());
    return handled;
  }

  bool toggleFullscreen() {
    if (finished)
      return false;
    const bool handled =
        dispatch(playback_session_input::CommandAction::ToggleFullscreen);
    if (handled)
      applyPresenterSync(syncPresentation());
    return handled;
  }

  bool activatePresentation() {
    if (finished)
      return false;
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

  void subtitlesLoaded(bool available, bool reload,
                       bool preferredTrackSelected) {
    hasSubtitles = available;
    subtitlesEnabled = available;
    redraw = true;
    forceRefreshArt = true;
    copiedFrameNeedsRender = true;
    startChapterAnalysis();
    if (initialized)
      syncOverlayPresentation();
    if (reload) {
      showEditMessage(preferredTrackSelected
                          ? "Subtitles ready and enabled"
                          : (available ? "Subtitles reloaded"
                                       : "Subtitle file could not be loaded"));
    }
  }

  void
  mediaTaskFinished(const playback_media_processing::Completion &completion) {
    const bool promptClosed = mediaActionConfirmation.synchronize(completion);
    if (promptClosed) {
      seekState.overlayControlPointer.reset();
    }
    const bool targetsCurrentSource = samePath(completion.sourceFile, file);
    if (targetsCurrentSource &&
        completion.operation ==
            playback_media_processing::Operation::SubtitleGeneration &&
        completion.succeeded()) {
      syncOverlayPresentation();
      showEditMessage("Transcript generated; loading as subtitles...");
      if (promptClosed)
        resumeDeferredNativeFileDrop();
      return;
    }
    syncOverlayPresentation();
    if (promptClosed)
      resumeDeferredNativeFileDrop();
    showEditMessage(playback_session::mediaTaskFeedback(
        completion, targetsCurrentSource ? std::filesystem::path{}
                                         : completion.sourceFile));
  }

  void mediaTaskActivityChanged(
      std::optional<playback_media_processing::Activity> activity) {
    const bool promptClosed = mediaActionConfirmation.synchronize(activity);
    if (mediaTaskActivity == activity && !promptClosed)
      return;
    mediaTaskActivity = std::move(activity);
    overlayControlHover = -1;
    if (promptClosed) {
      seekState.overlayControlPointer.reset();
    }
    syncOverlayPresentation();
    if (promptClosed)
      resumeDeferredNativeFileDrop();
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
    if (!finished)
      requestPlaybackExit(false);
  }

  void requestQuit() {
    if (!finished)
      requestPlaybackExit(true);
  }
};

PlaybackLoopRunner::PlaybackLoopRunner(Args args)
    : impl_(std::make_unique<Impl>(std::move(args))) {}

PlaybackLoopRunner::~PlaybackLoopRunner() = default;

PlaybackLoopRunner::PlaybackLoopRunner(PlaybackLoopRunner &&) noexcept =
    default;

PlaybackLoopRunner &
PlaybackLoopRunner::operator=(PlaybackLoopRunner &&) noexcept = default;

bool PlaybackLoopRunner::pump() { return impl_->pump(); }

PlaybackShellTerminalRole PlaybackLoopRunner::terminalRole() const {
  return impl_->terminalRole();
}

std::vector<NativeWaitHandle> PlaybackLoopRunner::activityWaitHandles() const {
  return impl_->activityWaitHandles();
}

wake_schedule::Deadline PlaybackLoopRunner::nextWakeDeadline() const {
  return impl_->nextWakeDeadline();
}

playback_session::ViewSnapshot PlaybackLoopRunner::viewSnapshot() const {
  return impl_->viewSnapshot();
}

bool PlaybackLoopRunner::capturesBrowserInput() const {
  return impl_->capturesBrowserInput();
}

void PlaybackLoopRunner::setExternalInputModal(bool modal) {
  impl_->setExternalInputModal(modal);
}

bool PlaybackLoopRunner::handleInputEvent(const InputEvent &event) {
  return impl_->handleInputEvent(event);
}

bool PlaybackLoopRunner::pollWindowInput(InputEvent &event) {
  return impl_->pollWindowInput(event);
}

bool PlaybackLoopRunner::handleWindowInputEvent(const InputEvent &event) {
  return impl_->handleWindowInputEvent(event);
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

bool PlaybackLoopRunner::abortHandoff(
    playback_session_exit::RequestId requestId) {
  return impl_->abortHandoff(requestId);
}

std::vector<playback_session::Event> PlaybackLoopRunner::drainEvents() {
  return impl_->drainEvents();
}

void PlaybackLoopRunner::mediaTaskFinished(
    const playback_media_processing::Completion &completion) {
  impl_->mediaTaskFinished(completion);
}

void PlaybackLoopRunner::mediaTaskActivityChanged(
    std::optional<playback_media_processing::Activity> activity) {
  impl_->mediaTaskActivityChanged(std::move(activity));
}

void PlaybackLoopRunner::subtitlesLoaded(bool available, bool reload,
                                         bool preferredTrackSelected) {
  impl_->subtitlesLoaded(available, reload, preferredTrackSelected);
}

void PlaybackLoopRunner::requestStop() { impl_->requestStop(); }

void PlaybackLoopRunner::requestQuit() { impl_->requestQuit(); }

void PlaybackLoopRunner::beginShutdown() { impl_->beginShutdown(); }

bool PlaybackLoopRunner::shutdownReady() { return impl_->shutdownReady(); }

bool PlaybackLoopRunner::finishShutdown() { return impl_->finishShutdown(); }

std::vector<NativeWaitHandle> PlaybackLoopRunner::shutdownWaitHandles() const {
  return impl_->shutdownWaitHandles();
}

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

const std::string &PlaybackLoopRunner::renderFailureMessage() const {
  return impl_->frameOutputState.renderFailMessage;
}

const std::string &PlaybackLoopRunner::renderFailureDetail() const {
  return impl_->frameOutputState.renderFailDetail;
}
