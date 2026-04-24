#include "presentation.h"

#include <atomic>
#include <memory>
#include <optional>
#include <utility>

#include "playback/video/player.h"
#include "playback/framebuffer/window_presenter.h"
#include "state.h"

namespace {

void resetWindowOverlayState(std::atomic<int64_t>& overlayUntilMs,
                             std::atomic<int>& overlayControlHover) {
  overlayUntilMs.store(0, std::memory_order_relaxed);
  overlayControlHover.store(-1, std::memory_order_relaxed);
}

}  // namespace

struct PlaybackPresentation::Impl {
  WindowPresenter windowPresenter;
  PlaybackLayout desiredLayout = PlaybackLayout::Terminal;
  PlaybackLayout activeLayout = PlaybackLayout::Terminal;
  std::optional<PlaybackSessionContinuationState> initialState;

  explicit Impl(PlaybackLayout initialLayout,
                std::optional<PlaybackSessionContinuationState> placement)
      : desiredLayout(initialLayout), initialState(std::move(placement)) {}

  bool windowRequested() const { return isWindowPlaybackLayout(desiredLayout); }

  bool windowActive() const { return isWindowPlaybackLayout(activeLayout); }
};

PlaybackPresentation::PlaybackPresentation(
    PlaybackLayout initialLayout,
    std::optional<PlaybackSessionContinuationState> initialState)
    : impl_(std::make_unique<Impl>(initialLayout, std::move(initialState))) {}

PlaybackPresentation::~PlaybackPresentation() = default;

PlaybackPresentation::PlaybackPresentation(PlaybackPresentation&&) noexcept =
    default;

PlaybackPresentation& PlaybackPresentation::operator=(
    PlaybackPresentation&&) noexcept = default;

bool PlaybackPresentation::windowRequested() const {
  return impl_->windowRequested();
}

bool PlaybackPresentation::windowActive() const { return impl_->windowActive(); }

bool PlaybackPresentation::consumeWindowCloseRequested() {
  return impl_->windowPresenter.consumeCloseRequested();
}

HANDLE PlaybackPresentation::windowCloseRequestedWaitHandle() const {
  return impl_->windowPresenter.closeRequestedWaitHandle();
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
    const std::function<bool()>& overlayVisible,
    const playback_framebuffer_presenter::TextGridPresentationProvider&
        buildTextGridPresentation,
    bool& redraw,
    bool& forceRefreshArt, std::atomic<int64_t>& overlayUntilMs,
    std::atomic<int>& overlayControlHover) {
  PlaybackPresenterSyncResult result;
  result.previousActiveLayout = impl_->activeLayout;
  if (impl_->windowRequested()) {
    const PlaybackSessionContinuationState* initialState =
        impl_->initialState ? &*impl_->initialState : nullptr;
    if (impl_->windowPresenter.start(player, buildUiState, overlayVisible,
                                     buildTextGridPresentation, initialState)) {
      impl_->activeLayout = PlaybackLayout::Window;
      impl_->initialState.reset();
    } else {
      impl_->desiredLayout = PlaybackLayout::Terminal;
      impl_->activeLayout = PlaybackLayout::Terminal;
      forceRefreshArt = true;
      redraw = true;
      resetWindowOverlayState(overlayUntilMs, overlayControlHover);
    }
  } else {
    if (impl_->windowActive() || impl_->windowPresenter.isOpen()) {
      resetWindowOverlayState(overlayUntilMs, overlayControlHover);
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

VideoWindow& PlaybackPresentation::window() {
  return impl_->windowPresenter.window();
}

const VideoWindow& PlaybackPresentation::window() const {
  return impl_->windowPresenter.window();
}

GpuVideoFrameCache& PlaybackPresentation::frameCache() {
  return impl_->windowPresenter.frameCache();
}

void PlaybackPresentation::requestPresent() {
  impl_->windowPresenter.requestPresent();
}
