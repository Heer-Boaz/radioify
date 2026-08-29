#include "output.h"

#include <memory>
#include <utility>

#include "playback/video/player.h"
#include "playback/video/framebuffer/frame_clipboard.h"
#include "playback/video/gpu/videoprocessor.h"
#include "playback/framebuffer/window_presenter.h"

struct PlaybackOutputController::Impl {
  Impl(Player& player, GpuRuntime& gpu, std::string mediaTitle,
       std::shared_ptr<playback_framebuffer_presenter::PresentationSource>
           presentationSource,
       SystemMediaCommandOwner systemMediaCommandOwner)
      : windowPresenter(player, gpu, std::move(mediaTitle),
                        std::move(presentationSource),
                        systemMediaCommandOwner) {}

  WindowPresenter windowPresenter;
  GpuVideoFrameCache terminalFrameCache;
};

PlaybackOutputController::PlaybackOutputController(
    Player& player, GpuRuntime& gpu, std::string mediaTitle,
    std::shared_ptr<playback_framebuffer_presenter::PresentationSource>
        presentationSource,
    SystemMediaCommandOwner systemMediaCommandOwner)
    : impl_(std::make_unique<Impl>(
          player, gpu, std::move(mediaTitle),
          std::move(presentationSource), systemMediaCommandOwner)) {}

PlaybackOutputController::~PlaybackOutputController() = default;

PlaybackOutputController::PlaybackOutputController(
    PlaybackOutputController&&) noexcept = default;

PlaybackOutputController& PlaybackOutputController::operator=(
    PlaybackOutputController&&) noexcept = default;

bool PlaybackOutputController::windowOpen() const {
  return windowLifecycle() == PlaybackWindowLifecycle::Open &&
         impl_->windowPresenter.isOpen();
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

PlaybackWindowLifecycle PlaybackOutputController::windowLifecycle() const {
  switch (impl_->windowPresenter.lifecycle()) {
    case WindowPresenter::Lifecycle::Closed:
      return PlaybackWindowLifecycle::Closed;
    case WindowPresenter::Lifecycle::Opening:
      return PlaybackWindowLifecycle::Opening;
    case WindowPresenter::Lifecycle::Open:
      return PlaybackWindowLifecycle::Open;
    case WindowPresenter::Lifecycle::Closing:
      return PlaybackWindowLifecycle::Closing;
    case WindowPresenter::Lifecycle::Failed:
      return PlaybackWindowLifecycle::Failed;
  }
  return PlaybackWindowLifecycle::Failed;
}

bool PlaybackOutputController::consumeWindowLifecycleChange() {
  return impl_->windowPresenter.consumeLifecycleChange();
}

bool PlaybackOutputController::requestOpenWindow() {
  return impl_->windowPresenter.requestStart();
}

void PlaybackOutputController::requestCloseWindow() {
  impl_->windowPresenter.requestStop();
}

bool PlaybackOutputController::windowCloseReady() const {
  return impl_->windowPresenter.stopReady();
}

bool PlaybackOutputController::finishCloseWindow() {
  return impl_->windowPresenter.finishStop();
}

NativeWaitHandle PlaybackOutputController::windowTransitionWaitHandle() const {
  switch (windowLifecycle()) {
    case PlaybackWindowLifecycle::Opening:
    case PlaybackWindowLifecycle::Open:
    case PlaybackWindowLifecycle::Failed:
      return impl_->windowPresenter.lifecycleWaitHandle();
    case PlaybackWindowLifecycle::Closing:
      return impl_->windowPresenter.stopWaitHandle();
    case PlaybackWindowLifecycle::Closed:
      return {};
  }
  return {};
}

NativeWaitHandle PlaybackOutputController::windowShutdownWaitHandle() const {
  return impl_->windowPresenter.stopWaitHandle();
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
