#include "output.h"

#include <memory>
#include <utility>

#include "playback/video/player.h"
#include "playback/video/framebuffer/frame_clipboard.h"
#include "playback/video/gpu/videoprocessor.h"
#include "playback/ascii/screen_renderer.h"
#include "playback/framebuffer/window_presenter.h"

struct PlaybackOutputController::Impl {
  WindowPresenter windowPresenter;
  GpuVideoFrameCache terminalFrameCache;
};

PlaybackOutputController::PlaybackOutputController()
    : impl_(std::make_unique<Impl>()) {}

PlaybackOutputController::~PlaybackOutputController() = default;

PlaybackOutputController::PlaybackOutputController(
    PlaybackOutputController&&) noexcept = default;

PlaybackOutputController& PlaybackOutputController::operator=(
    PlaybackOutputController&&) noexcept = default;

bool PlaybackOutputController::windowOpen() const {
  return impl_->windowPresenter.isOpen();
}

bool PlaybackOutputController::windowVisible() const {
  return impl_->windowPresenter.isVisible();
}

bool PlaybackOutputController::consumeWindowCloseRequested() {
  return impl_->windowPresenter.consumeCloseRequested();
}

NativeWaitHandle PlaybackOutputController::windowInputWaitHandle() const {
  return impl_->windowPresenter.window().InputWaitHandle();
}

NativeWaitHandle
PlaybackOutputController::windowCloseRequestedWaitHandle() const {
  return impl_->windowPresenter.closeRequestedWaitHandle();
}

bool PlaybackOutputController::openWindow(
    Player& player, const std::string& mediaTitle,
    const std::function<WindowUiState()>& buildUiState,
    const playback_framebuffer_presenter::TextGridPresentationProvider&
        buildTextGridPresentation) {
  return impl_->windowPresenter.start(player, mediaTitle, buildUiState,
                                      buildTextGridPresentation);
}

void PlaybackOutputController::closeWindow() {
  impl_->windowPresenter.stop();
}

bool PlaybackOutputController::pollWindowInput(InputEvent& event) {
  return impl_->windowPresenter.window().PollInput(event);
}

void PlaybackOutputController::updateWindowCursor(
    Player& player, PlaybackSessionState playbackState, bool overlayVisible) {
  if (impl_->windowPresenter.isOpen() && impl_->windowPresenter.isVisible()) {
    const PlayerState state = player.state();
    const bool isActivelyPlaying =
        state == PlayerState::Playing || state == PlayerState::Draining;
    const bool showCursor = overlayVisible || playbackState == PlaybackSessionState::Paused ||
                            !isActivelyPlaying;
    impl_->windowPresenter.setCursorVisible(showCursor);
    return;
  }
  impl_->windowPresenter.setCursorVisible(true);
}

void PlaybackOutputController::renderTerminal(
    playback_screen_renderer::PlaybackScreenRenderInputs& inputs) {
  playback_screen_renderer::renderPlaybackScreen(inputs);
}

bool PlaybackOutputController::applyWindowPresentation(
    WindowPresentationRequest request) {
  return impl_->windowPresenter.applyPresentation(request);
}

bool PlaybackOutputController::restoreWindowPresentation(
    WindowPresentationRequest request,
    const WindowPlacementState& placement) {
  return impl_->windowPresenter.restorePresentation(request, placement);
}

bool PlaybackOutputController::captureWindowPlacement(
    WindowPlacementState& placement) {
  return impl_->windowPresenter.capturePlacement(placement);
}

bool PlaybackOutputController::activateWindow() {
  return impl_->windowPresenter.activate();
}

VideoWindow& PlaybackOutputController::window() {
  return impl_->windowPresenter.window();
}

const VideoWindow& PlaybackOutputController::window() const {
  return impl_->windowPresenter.window();
}

GpuVideoFrameCache& PlaybackOutputController::frameCache() {
  return impl_->terminalFrameCache;
}

void PlaybackOutputController::requestWindowPresent() {
  impl_->windowPresenter.requestPresent();
}

bool PlaybackOutputController::copyCurrentVideoFrameToClipboard(
    std::string* error) {
  VideoFrameSnapshotResult result =
      impl_->windowPresenter.captureCurrentFrame();
  if (!result.succeeded()) {
    if (error) {
      *error = std::move(result.error);
    }
    return false;
  }
  return playback_video_frame_clipboard::copyToClipboard(
      impl_->windowPresenter.nativeWindowHandle(), result.snapshot, error);
}
