#include "presentation_model.h"

#include <utility>

#include "playback/ascii/screen_renderer.h"
#include "playback/video/player.h"
#include "playback/video/subtitle/manager.h"

namespace playback_session {
namespace {

struct PublishedState {
  playback_framebuffer_presenter::PlaybackFramebufferUiSnapshot ui;
  PlaybackSessionState playbackState = PlaybackSessionState::Active;
  bool audioOk = false;
  bool audioStarting = false;
  bool hasSubtitles = false;
  bool debugOverlay = false;
  playback_screen_renderer::PlaybackScreenRenderInputs textGridInputs;
  bool hasTextGridInputs = false;
};

}  // namespace

struct PresentationModel::Impl {
  Impl(Dependencies dependencies, FixedState fixedState)
      : dependencies(dependencies), fixedState(std::move(fixedState)) {}

  Dependencies dependencies;
  const FixedState fixedState;
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

PresentationModel::PresentationModel(Dependencies dependencies,
                                     FixedState fixedState)
    : impl_(std::make_unique<Impl>(dependencies, std::move(fixedState))) {}

PresentationModel::~PresentationModel() = default;

void PresentationModel::publishWindowState(
    playback_framebuffer_presenter::PlaybackFramebufferUiSnapshot ui,
    PlaybackSessionState playbackState, bool audioOk, bool audioStarting,
    bool hasSubtitles, bool debugOverlay) {
  std::lock_guard<std::mutex> lock(impl_->publishedMutex);
  impl_->published.ui = std::move(ui);
  impl_->published.playbackState = playbackState;
  impl_->published.audioOk = audioOk;
  impl_->published.audioStarting = audioStarting;
  impl_->published.hasSubtitles = hasSubtitles;
  impl_->published.debugOverlay = debugOverlay;
}

void PresentationModel::publishTextGridInputs(
    const playback_screen_renderer::PlaybackScreenRenderInputs& inputs) {
  std::lock_guard<std::mutex> lock(impl_->publishedMutex);
  impl_->published.textGridInputs = inputs;
  impl_->published.hasTextGridInputs = true;
}

WindowUiState PresentationModel::buildWindowUiState(
    VideoWindow& videoWindow) {
  const PublishedState state = impl_->snapshot();
  std::lock_guard<std::mutex> subtitleLock(
      impl_->dependencies.subtitleMutex);
  return playback_framebuffer_presenter::buildPlaybackFramebufferUiState(
      impl_->fixedState.windowTitle, videoWindow, impl_->dependencies.player,
      impl_->dependencies.subtitleManager, state.playbackState, state.audioOk,
      impl_->fixedState.canPlayPrevious, impl_->fixedState.canPlayNext,
      state.hasSubtitles, impl_->dependencies.subtitlesEnabled,
      impl_->dependencies.controlHover, state.ui, state.debugOverlay);
}

bool PresentationModel::renderTextGrid(
    const playback_framebuffer_presenter::TextGridPresentationRequest&
        request,
    playback_framebuffer_presenter::TextGridPresentationTarget target) {
  const PublishedState state = impl_->snapshot();
  if (!state.hasTextGridInputs) return false;

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

  playback_screen_renderer::PlaybackScreenRenderInputs inputs =
      state.textGridInputs;
  inputs.screen = &impl_->textGridScreen;
  inputs.videoWindow = &request.videoWindow;
  inputs.frame = &impl_->textGridFrame;
  inputs.frameCache = &impl_->textGridFrameCache;
  inputs.art = &impl_->textGridArt;
  inputs.timelinePreviewCache = &impl_->timelinePreviewArt;
  inputs.visualMode = PlaybackVisualMode::AsciiGrid;
  inputs.debugOverlay = state.debugOverlay;
  inputs.playbackState = state.playbackState;
  inputs.audioOk = state.audioOk;
  inputs.audioStarting = state.audioStarting;
  inputs.hasSubtitles = state.hasSubtitles;
  inputs.nativeWindowActive = false;
  const bool audioOnlyPlayback = impl_->dependencies.player.sourceWidth() <= 0 ||
                                 impl_->dependencies.player.sourceHeight() <= 0;
  inputs.osd = state.ui.osd;
  inputs.timelinePreview = state.ui.timelinePreview;
  inputs.videoEdit = state.ui.videoEdit;
  if (inputs.videoEdit.active) {
    inputs.videoEdit.playheadTimelineUs =
        impl_->dependencies.player.timelineSnapshot().positionUs;
  }
  inputs.videoEditExport = state.ui.videoEditExport;
  inputs.videoEditPrompt = state.ui.videoEditPrompt;
  inputs.contextMenu = state.ui.contextMenu;
  inputs.osd.controlsVisible = inputs.osd.controlsVisible || audioOnlyPlayback;
  inputs.clearHistory = false;
  inputs.frameChanged = request.frameChanged;
  inputs.cellPixelWidth = request.cellPixelWidth;
  inputs.cellPixelHeight = request.cellPixelHeight;
  inputs.cellPixelSourceLabel = "text-grid-presentation";
  inputs.allowAsciiCpuFallback = false;
  inputs.debugLines.clear();
  if (state.debugOverlay) {
    inputs.debugLines.push_back(request.videoWindow.OutputColorDebugLine());
    if (!request.enhancementDebugLine.empty()) {
      inputs.debugLines.push_back(request.enhancementDebugLine);
    }
  }
  inputs.frameOutputState = &outputState;
  inputs.frameAvailable = outputState.haveFrame;

  playback_screen_renderer::renderPlaybackScreen(inputs);
  if (outputState.renderFailed) return false;

  target.interactions = outputState.overlayInteractions;
  return impl_->textGridScreen.snapshot(target.cells, target.cols,
                                         target.rows);
}

}  // namespace playback_session
