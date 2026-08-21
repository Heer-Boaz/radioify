#include "output.h"

#include <memory>
#include <utility>

#include "playback/video/player.h"
#include "playback/video/framebuffer/frame_clipboard.h"
#include "playback/video/gpu/videoprocessor.h"
#include "playback/ascii/screen_renderer.h"
#include "presentation.h"

struct PlaybackOutputController::Impl {
  explicit Impl(PlaybackLayout initialLayout) : presentation(initialLayout) {}

  PlaybackPresentation presentation;
  GpuVideoFrameCache terminalFrameCache;
};

PlaybackOutputController::PlaybackOutputController(
    PlaybackLayout initialLayout)
    : impl_(std::make_unique<Impl>(initialLayout)) {}

PlaybackOutputController::~PlaybackOutputController() = default;

PlaybackOutputController::PlaybackOutputController(
    PlaybackOutputController&&) noexcept = default;

PlaybackOutputController& PlaybackOutputController::operator=(
    PlaybackOutputController&&) noexcept = default;

bool PlaybackOutputController::windowRequested() const {
  return impl_->presentation.windowRequested();
}

bool PlaybackOutputController::windowActive() const {
  return impl_->presentation.windowActive();
}

bool PlaybackOutputController::windowOpen() const {
  return impl_->presentation.windowOpen();
}

bool PlaybackOutputController::windowVisible() const {
  return impl_->presentation.windowVisible();
}

bool PlaybackOutputController::consumeWindowCloseRequested() {
  return impl_->presentation.consumeWindowCloseRequested();
}

NativeWaitHandle PlaybackOutputController::windowInputWaitHandle() const {
  return impl_->presentation.windowInputWaitHandle();
}

NativeWaitHandle
PlaybackOutputController::windowCloseRequestedWaitHandle() const {
  return impl_->presentation.windowCloseRequestedWaitHandle();
}

PlaybackRenderMode PlaybackOutputController::renderMode(bool enableAscii) const {
  return impl_->presentation.renderMode(enableAscii);
}

void PlaybackOutputController::requestLayout(PlaybackLayout layout) {
  impl_->presentation.requestLayout(layout);
}

PlaybackLayout PlaybackOutputController::desiredLayout() const {
  return impl_->presentation.desiredLayout();
}

PlaybackPresenterSyncResult PlaybackOutputController::sync(
    Player& player,
    const std::function<WindowUiState()>& buildUiState,
    const playback_framebuffer_presenter::TextGridPresentationProvider&
        buildTextGridPresentation,
    bool& redraw, bool& forceRefreshArt) {
  return impl_->presentation.sync(player, buildUiState,
                                  buildTextGridPresentation, redraw,
                                  forceRefreshArt);
}

bool PlaybackOutputController::pollWindowInput(InputEvent& event) {
  return impl_->presentation.window().PollInput(event);
}

void PlaybackOutputController::updateWindowCursor(
    Player& player, PlaybackSessionState playbackState, bool overlayVisible) {
  if (impl_->presentation.windowActive() &&
      impl_->presentation.windowVisible()) {
    const PlayerState state = player.state();
    const bool isActivelyPlaying =
        state == PlayerState::Playing || state == PlayerState::Draining;
    const bool showCursor = overlayVisible || playbackState == PlaybackSessionState::Paused ||
                            !isActivelyPlaying;
    impl_->presentation.setWindowCursorVisible(showCursor);
    return;
  }
  impl_->presentation.setWindowCursorVisible(true);
}

void PlaybackOutputController::renderTerminal(
    playback_screen_renderer::PlaybackScreenRenderInputs& inputs) {
  playback_screen_renderer::renderPlaybackScreen(inputs);
}

void PlaybackOutputController::stop() {
  impl_->presentation.stop();
}

bool PlaybackOutputController::applyWindowPresentation(
    PlaybackWindowPresentationRequest request) {
  return impl_->presentation.applyWindowPresentation(request);
}

bool PlaybackOutputController::restoreWindowPresentation(
    PlaybackWindowPresentationRequest request,
    const WindowPlacementState& placement) {
  return impl_->presentation.restoreWindowPresentation(request, placement);
}

bool PlaybackOutputController::captureWindowPlacement(
    WindowPlacementState& placement,
    const PlaybackPresentationState& presentation) {
  return impl_->presentation.captureWindowPlacement(placement, presentation);
}

bool PlaybackOutputController::activateWindow() {
  return impl_->presentation.activateWindow();
}

VideoWindow& PlaybackOutputController::window() {
  return impl_->presentation.window();
}

const VideoWindow& PlaybackOutputController::window() const {
  return impl_->presentation.window();
}

GpuVideoFrameCache& PlaybackOutputController::frameCache() {
  return impl_->terminalFrameCache;
}

void PlaybackOutputController::requestWindowPresent() {
  impl_->presentation.requestPresent();
}

bool PlaybackOutputController::copyCurrentVideoFrameToClipboard(
    std::string* error) {
  VideoFrameSnapshotResult result =
      impl_->presentation.captureCurrentFrame();
  if (!result.succeeded()) {
    if (error) {
      *error = std::move(result.error);
    }
    return false;
  }
  return playback_video_frame_clipboard::copyToClipboard(
      impl_->presentation.nativeWindowHandle(), result.snapshot, error);
}
