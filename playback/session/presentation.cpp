#include "presentation.h"

#include <memory>

#include "playback/video/player.h"
#include "playback/framebuffer/window_presenter.h"
#include "state.h"

struct PlaybackPresentation::Impl {
  WindowPresenter windowPresenter;
  PlaybackLayout desiredLayout = PlaybackLayout::Terminal;
  PlaybackLayout activeLayout = PlaybackLayout::Terminal;

  explicit Impl(PlaybackLayout initialLayout) : desiredLayout(initialLayout) {}

  bool windowRequested() const { return isWindowPlaybackLayout(desiredLayout); }

  bool windowActive() const { return isWindowPlaybackLayout(activeLayout); }
};

PlaybackPresentation::PlaybackPresentation(
    PlaybackLayout initialLayout)
    : impl_(std::make_unique<Impl>(initialLayout)) {}

PlaybackPresentation::~PlaybackPresentation() = default;

PlaybackPresentation::PlaybackPresentation(PlaybackPresentation&&) noexcept =
    default;

PlaybackPresentation& PlaybackPresentation::operator=(
    PlaybackPresentation&&) noexcept = default;

bool PlaybackPresentation::windowRequested() const {
  return impl_->windowRequested();
}

bool PlaybackPresentation::windowActive() const { return impl_->windowActive(); }

bool PlaybackPresentation::windowOpen() const {
  return impl_->windowPresenter.isOpen();
}

bool PlaybackPresentation::windowVisible() const {
  return impl_->windowPresenter.isVisible();
}

HWND PlaybackPresentation::nativeWindowHandle() const {
  return impl_->windowPresenter.nativeWindowHandle();
}

bool PlaybackPresentation::consumeWindowCloseRequested() {
  return impl_->windowPresenter.consumeCloseRequested();
}

NativeWaitHandle PlaybackPresentation::windowCloseRequestedWaitHandle() const {
  return impl_->windowPresenter.closeRequestedWaitHandle();
}

NativeWaitHandle PlaybackPresentation::windowInputWaitHandle() const {
  return impl_->windowPresenter.window().InputWaitHandle();
}

PlaybackRenderMode PlaybackPresentation::renderMode(bool enableAscii) const {
  return resolvePlaybackMode(enableAscii, impl_->activeLayout);
}

void PlaybackPresentation::requestLayout(PlaybackLayout layout) {
  impl_->desiredLayout = layout;
}

PlaybackLayout PlaybackPresentation::desiredLayout() const {
  return impl_->desiredLayout;
}

PlaybackPresenterSyncResult PlaybackPresentation::sync(
    Player& player,
    const std::function<WindowUiState()>& buildUiState,
    const playback_framebuffer_presenter::TextGridPresentationProvider&
        buildTextGridPresentation,
    bool& redraw, bool& forceRefreshArt) {
  PlaybackPresenterSyncResult result;
  result.previousActiveLayout = impl_->activeLayout;
  if (impl_->windowRequested()) {
    if (impl_->windowPresenter.start(player, buildUiState,
                                     buildTextGridPresentation)) {
      impl_->activeLayout = PlaybackLayout::Window;
    } else {
      impl_->desiredLayout = PlaybackLayout::Terminal;
      impl_->activeLayout = PlaybackLayout::Terminal;
      result.windowStartFailed = true;
      forceRefreshArt = true;
      redraw = true;
    }
  } else {
    if (impl_->windowActive() || impl_->windowPresenter.isOpen()) {
      impl_->windowPresenter.stop();
    }
    impl_->activeLayout = PlaybackLayout::Terminal;
  }
  result.activeLayout = impl_->activeLayout;
  return result;
}

void PlaybackPresentation::stop() {
  impl_->windowPresenter.stop();
  impl_->activeLayout = PlaybackLayout::Terminal;
}

bool PlaybackPresentation::applyWindowPresentation(
    PlaybackWindowPresentationRequest request) {
  return impl_->windowPresenter.applyPresentation(request);
}

bool PlaybackPresentation::restoreWindowPresentation(
    PlaybackWindowPresentationRequest request,
    const WindowPlacementState& placement) {
  return impl_->windowPresenter.restorePresentation(request, placement);
}

bool PlaybackPresentation::captureWindowPlacement(
    WindowPlacementState& placement,
    const PlaybackPresentationState& presentation) {
  return impl_->windowPresenter.capturePlacement(placement, presentation);
}

bool PlaybackPresentation::activateWindow() {
  return impl_->windowPresenter.activate();
}

void PlaybackPresentation::setWindowCursorVisible(bool visible) {
  impl_->windowPresenter.setCursorVisible(visible);
}

VideoWindow& PlaybackPresentation::window() {
  return impl_->windowPresenter.window();
}

const VideoWindow& PlaybackPresentation::window() const {
  return impl_->windowPresenter.window();
}

void PlaybackPresentation::requestPresent() {
  impl_->windowPresenter.requestPresent();
}

VideoFrameSnapshotResult PlaybackPresentation::captureCurrentFrame() {
  return impl_->windowPresenter.captureCurrentFrame();
}
