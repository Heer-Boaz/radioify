#include "presentation_model.h"

#include <mutex>
#include <utility>

#include "playback/ascii/screen_renderer.h"
#include "presentation_projector.h"

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

WindowUiState
PresentationModel::windowUiState(const PlayerTimelineSnapshot &timeline) {
  const PublishedState state = impl_->snapshot();
  if (!state.available) return {};
  return projectWindowUiState(state.revision.textGrid, timeline);
}

bool PresentationModel::renderTextGrid(
    const playback_framebuffer_presenter::TextGridPresentationRequest&
        request,
    playback_framebuffer_presenter::TextGridPresentationTarget target) {
  const PublishedState state = impl_->snapshot();
  if (!state.available) return false;

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
  model.media.timeline = request.timeline;
  projectPlaybackTimeline(model.overlay, model.media, request.timeline);
  model.visualMode = PlaybackVisualMode::AsciiGrid;
  model.nativeWindowActive = false;
  const bool audioOnlyPlayback = model.media.sourceWidth <= 0 ||
                                 model.media.sourceHeight <= 0;
  if (audioOnlyPlayback) {
    model.overlay.overlayVisible = true;
    model.overlay.chromeVisible = true;
  }
  model.clearHistory = false;
  model.frameChanged = request.frameChanged;
  model.cellPixelWidth = request.cellPixelWidth;
  model.cellPixelHeight = request.cellPixelHeight;
  model.cellPixelSourceLabel = "text-grid-presentation";
  model.allowAsciiCpuFallback = false;
  model.overlay.debugLines.clear();
  if (model.debugOverlay) {
    model.overlay.debugLines.push_back(
        request.videoWindow.OutputColorDebugLine());
    if (!request.enhancementDebugLine.empty()) {
      model.overlay.debugLines.push_back(request.enhancementDebugLine);
    }
    model.overlay.chromeVisible = true;
  }
  model.frameAvailable = outputState.haveFrame;

  playback_screen_renderer::PlaybackScreenTarget renderTarget{
      impl_->textGridScreen, impl_->textGridFrameCache, impl_->textGridArt,
      impl_->timelinePreviewArt, impl_->textGridFrame, outputState};
  playback_screen_renderer::renderPlaybackScreen(
      impl_->dependencies.renderer, renderTarget, model);
  if (outputState.renderFailed) return false;

  target.interactions = outputState.overlayInteractions;
  return impl_->textGridScreen.snapshot(target.cells, target.cols,
                                         target.rows);
}

}  // namespace playback_session
