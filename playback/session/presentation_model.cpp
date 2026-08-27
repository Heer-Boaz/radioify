#include "presentation_model.h"

#include <utility>

#include "playback/ascii/screen_renderer.h"
#include "playback/video/player.h"
#include "playback/video/subtitle/manager.h"

namespace playback_session {
namespace {

struct PublishedState {
  PresentationModel::Revision revision;
  bool available = false;
};

}  // namespace

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

WindowUiState PresentationModel::buildWindowUiState(
    VideoWindow& videoWindow) {
  const PublishedState state = impl_->snapshot();
  if (!state.available) return {};
  const auto& model = state.revision.textGrid;
  const auto& renderer = impl_->dependencies.renderer;
  std::lock_guard<std::mutex> subtitleLock(
      impl_->dependencies.subtitleMutex);
  return playback_framebuffer_presenter::buildPlaybackFramebufferUiState(
      renderer.windowTitle, videoWindow, renderer.player,
      renderer.subtitleManager, model.playbackState, model.audioOk,
      model.canPlayPrevious, model.canPlayNext, model.hasSubtitles,
      renderer.subtitlesEnabled, renderer.controlHover,
      state.revision.window, model.debugOverlay);
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
  model.osd = state.revision.window.osd;
  model.timelinePreview = state.revision.window.timelinePreview;
  model.videoEdit = state.revision.window.videoEdit;
  if (model.videoEdit.active) {
    model.videoEdit.playheadTimelineUs =
        renderer.player.timelineSnapshot().positionUs;
  }
  model.videoEditExport = state.revision.window.videoEditExport;
  model.videoEditPrompt = state.revision.window.videoEditPrompt;
  model.contextMenu = state.revision.window.contextMenu;
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
