#include "presentation_model.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "audio/audioplayback.h"
#include "playback/ascii/screen_renderer.h"
#include "playback/debug/lines.h"
#include "playback/video/player.h"
#include "playback/video/state/machine.h"
#include "playback/video/subtitle/manager.h"

namespace playback_session {
namespace {

struct PublishedState {
  PresentationModel::Revision revision;
  bool available = false;
};

}  // namespace

WindowUiState projectWindowUiState(
    const playback_screen_renderer::PlaybackScreenResources& resources,
    VideoWindow& videoWindow,
    const playback_screen_renderer::PlaybackScreenModel& playback,
    const WindowPresentationModel& window) {
  Player& player = resources.player;
  const AudioPlaybackSnapshot audio = resources.audioPlayback.snapshot();
  const PlayerTimelineSnapshot timeline = player.timelineSnapshot();
  double displaySec = timeline.positionUs > 0
                          ? static_cast<double>(timeline.positionUs) /
                                1000000.0
                          : 0.0;
  const int64_t durationUs = player.durationUs();
  double totalSec = durationUs > 0
                        ? static_cast<double>(durationUs) / 1000000.0
                        : (playback.audioOk ? audio.durationSec : -1.0);
  if (totalSec > 0.0) {
    displaySec = std::clamp(displaySec, 0.0, totalSec);
  }
  const bool playerPaused =
      playback_video_state_machine::project(player.state()).transport ==
      playback_video_state_machine::TransportState::Paused;

  playback_overlay::PlaybackOverlayInputs inputs;
  inputs.windowTitle = resources.windowTitle;
  inputs.audioOk = playback.audioOk;
  inputs.playPauseAvailable =
      playback.playbackState == PlaybackSessionState::Active ||
      playback.playbackState == PlaybackSessionState::Paused ||
      playback.playbackState == PlaybackSessionState::Ended;
  inputs.audioSupports50HzToggle =
      playback.audioOk && audio.supports50HzToggle;
  inputs.canPlayPrevious = playback.canPlayPrevious;
  inputs.canPlayNext = playback.canPlayNext;
  inputs.radioEnabled = audio.radioEnabled;
  inputs.radioLabel = std::string(audio.radioFilterLabel);
  inputs.hz50Enabled = audio.hz50Enabled;
  inputs.canCycleAudioTracks =
      playback.audioOk && player.canCycleAudioTracks();
  inputs.activeAudioTrackLabel =
      playback.audioOk ? player.activeAudioTrackLabel() : "N/A";
  inputs.subtitleManager = &resources.subtitleManager;
  inputs.hasSubtitles = playback.hasSubtitles;
  inputs.subtitlesEnabled =
      resources.subtitlesEnabled.load(std::memory_order_relaxed);
  inputs.subtitleClockUs = timeline.sourcePositionUs;
  inputs.seekingOverlay = timeline.seekPending();
  inputs.displaySec = displaySec;
  inputs.totalSec = totalSec;
  inputs.volPct =
      static_cast<int>(std::round(audio.volume * 100.0f));
  inputs.osd = window.osd;
  inputs.paused = playback.playbackState == PlaybackSessionState::Paused ||
                  playback.playbackState == PlaybackSessionState::Ended ||
                  player.isEnded() || playerPaused;
  inputs.pictureInPictureAvailable = videoWindow.IsOpen();
  inputs.pictureInPictureActive =
      inputs.pictureInPictureAvailable && videoWindow.IsPictureInPicture();
  inputs.subtitleRenderError = videoWindow.GetSubtitleRenderError();
  inputs.contextMenu = window.contextMenu;
  inputs.videoEdit = window.videoEdit;
  if (inputs.videoEdit.active) {
    inputs.videoEdit.playheadTimelineUs = timeline.positionUs;
  }
  inputs.videoEditExport = window.videoEditExport;
  inputs.videoEditPrompt = window.videoEditPrompt;

  WindowUiState ui = playback_overlay::buildWindowUiState(
      playback_overlay::buildPlaybackOverlayState(inputs),
      resources.controlHover.load(std::memory_order_relaxed));
  ui.timelinePreview = window.timelinePreview;
  if (playback.debugOverlay) {
    ui.debugLines.push_back(videoWindow.OutputColorDebugLine());
    ui.debugLines.push_back(
        playback_debug_lines::videoFrameDebugLine(player.debugInfo()));
  }
  return ui;
}

struct PresentationModel::Impl {
  explicit Impl(Dependencies dependencies) : dependencies(dependencies) {}

  Dependencies dependencies;
  std::mutex publishedMutex;
  PublishedState published;

  ConsoleScreen textGridScreen;
  AsciiArt textGridArt;
  playback_screen_renderer::TimelinePreviewAsciiCache timelinePreviewArt;
  VideoFrame textGridFrame;
  GpuVideoFrameCache textGridFrameCache;
  playback_frame_output::FrameOutputState textGridOutputState;

  PublishedState snapshot() {
    std::lock_guard<std::mutex> lock(publishedMutex);
    return published;
  }
};

PresentationModel::PresentationModel(Dependencies dependencies)
    : impl_(std::make_unique<Impl>(dependencies)) {}

PresentationModel::~PresentationModel() = default;

void PresentationModel::publish(Revision revision) {
  std::lock_guard<std::mutex> lock(impl_->publishedMutex);
  impl_->published.revision = std::move(revision);
  impl_->published.available = true;
}

WindowUiState PresentationModel::windowUiState() {
  const PublishedState state = impl_->snapshot();
  if (!state.available) return {};
  return state.revision.window;
}

bool PresentationModel::renderTextGrid(
    const playback_framebuffer_presenter::TextGridPresentationRequest&
        request,
    playback_framebuffer_presenter::TextGridPresentationTarget target) {
  const PublishedState state = impl_->snapshot();
  if (!state.available) return false;

  std::lock_guard<std::mutex> subtitleLock(
      impl_->dependencies.subtitleMutex);
  const int cols = playback_overlay::overlayCellCountForPixels(
      request.pixelWidth, request.cellPixelWidth);
  const int rows = playback_overlay::overlayCellCountForPixels(
      request.pixelHeight, request.cellPixelHeight);
  impl_->textGridScreen.setVirtualSize(cols, rows);

  auto& outputState = impl_->textGridOutputState;
  outputState.renderFailed = false;
  outputState.renderFailMessage.clear();
  outputState.renderFailDetail.clear();
  if (request.frame && request.frame->width > 0 && request.frame->height > 0) {
    impl_->textGridFrame = *request.frame;
    outputState.haveFrame = true;
  } else if (!outputState.haveFrame) {
    impl_->textGridFrame = VideoFrame{};
  }

  playback_screen_renderer::PlaybackScreenModel model =
      state.revision.textGrid;
  model.visualMode = PlaybackVisualMode::AsciiGrid;
  model.nativeWindowActive = false;
  const auto& renderer = impl_->dependencies.renderer;
  const bool audioOnlyPlayback = renderer.player.sourceWidth() <= 0 ||
                                 renderer.player.sourceHeight() <= 0;
  model.osd = state.revision.windowModel.osd;
  model.timelinePreview = state.revision.windowModel.timelinePreview;
  model.videoEdit = state.revision.windowModel.videoEdit;
  if (model.videoEdit.active) {
    model.videoEdit.playheadTimelineUs =
        renderer.player.timelineSnapshot().positionUs;
  }
  model.videoEditExport = state.revision.windowModel.videoEditExport;
  model.videoEditPrompt = state.revision.windowModel.videoEditPrompt;
  model.contextMenu = state.revision.windowModel.contextMenu;
  model.osd.controlsVisible = model.osd.controlsVisible || audioOnlyPlayback;
  model.clearHistory = false;
  model.frameChanged = request.frameChanged;
  model.cellPixelWidth = request.cellPixelWidth;
  model.cellPixelHeight = request.cellPixelHeight;
  model.cellPixelSourceLabel = "text-grid-presentation";
  model.allowAsciiCpuFallback = false;
  model.debugLines.clear();
  if (model.debugOverlay) {
    model.debugLines.push_back(request.videoWindow.OutputColorDebugLine());
    if (!request.enhancementDebugLine.empty()) {
      model.debugLines.push_back(request.enhancementDebugLine);
    }
  }
  model.frameAvailable = outputState.haveFrame;

  playback_screen_renderer::PlaybackScreenTarget renderTarget{
      impl_->textGridScreen, request.videoWindow, impl_->textGridFrameCache,
      impl_->textGridArt, impl_->timelinePreviewArt, impl_->textGridFrame,
      outputState};
  playback_screen_renderer::renderPlaybackScreen(renderer, renderTarget,
                                                 model);
  if (outputState.renderFailed) return false;

  target.interactions = outputState.overlayInteractions;
  return impl_->textGridScreen.snapshot(target.cells, target.cols,
                                         target.rows);
}

}  // namespace playback_session
