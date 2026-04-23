#include "output.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "consoleinput.h"
#include "core/windows_message_pump.h"
#include "player.h"
#include "playback/ascii/screen_renderer.h"
#include "presentation.h"

struct PlaybackOutputController::Impl {
  explicit Impl(PlaybackLayout initialLayout,
                std::optional<PlaybackSessionContinuationState> initialState)
      : presentation(initialLayout, std::move(initialState)) {}

  PlaybackPresentation presentation;
};

PlaybackOutputController::PlaybackOutputController(
    PlaybackLayout initialLayout,
    std::optional<PlaybackSessionContinuationState> initialState)
    : impl_(std::make_unique<Impl>(initialLayout, std::move(initialState))) {}

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

bool PlaybackOutputController::windowVisible() const {
  return impl_->presentation.window().IsVisible();
}

bool PlaybackOutputController::consumeWindowCloseRequested() {
  return impl_->presentation.consumeWindowCloseRequested();
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
    const std::function<bool()>& overlayVisible,
    const playback_framebuffer_presenter::TextGridPresentationProvider&
        buildTextGridPresentation,
    bool& redraw,
    bool& forceRefreshArt, std::atomic<int64_t>& overlayUntilMs,
    std::atomic<int>& overlayControlHover) {
  return impl_->presentation.sync(player, buildUiState, overlayVisible,
                                  buildTextGridPresentation, redraw,
                                  forceRefreshArt, overlayUntilMs,
                                  overlayControlHover);
}

bool PlaybackOutputController::pollInput(ConsoleInput& input, InputEvent& ev) {
  if (input.poll(ev)) {
    return true;
  }
  return impl_->presentation.window().IsOpen() &&
         impl_->presentation.window().PollInput(ev);
}

bool PlaybackOutputController::waitForActivity(ConsoleInput& input, int timeoutMs,
                                               HANDLE extraHandle) {
  HANDLE handles[3];
  DWORD handleCount = 0;
  if (HANDLE inputHandle = input.waitHandle()) {
    handles[handleCount++] = inputHandle;
  }
  if (extraHandle) {
    handles[handleCount++] = extraHandle;
  }
  if (impl_->presentation.windowActive()) {
    if (HANDLE closeHandle = impl_->presentation.windowCloseRequestedWaitHandle()) {
      handles[handleCount++] = closeHandle;
    }
  }

  DWORD waitMs =
      timeoutMs < 0 ? INFINITE : static_cast<DWORD>(std::max(0, timeoutMs));
  DWORD result = waitForHandlesAndPumpThreadWindowMessages(
      handleCount, handleCount > 0 ? handles : nullptr, waitMs);
  return result != WAIT_TIMEOUT && result != WAIT_FAILED;
}

void PlaybackOutputController::updateWindowCursor(
    Player& player, PlaybackSessionState playbackState, bool overlayVisible) {
  if (impl_->presentation.windowActive() &&
      impl_->presentation.window().IsOpen() &&
      impl_->presentation.window().IsVisible()) {
    const PlayerState state = player.state();
    const bool isActivelyPlaying =
        state == PlayerState::Playing || state == PlayerState::Draining;
    const bool showCursor = overlayVisible || playbackState == PlaybackSessionState::Paused ||
                            !isActivelyPlaying;
    impl_->presentation.window().SetCursorVisible(showCursor);
    return;
  }
  impl_->presentation.window().SetCursorVisible(true);
}

void PlaybackOutputController::renderTerminal(
    playback_screen_renderer::PlaybackScreenRenderInputs& inputs) {
  playback_screen_renderer::renderPlaybackScreen(inputs);
}

void PlaybackOutputController::stop() {
  impl_->presentation.stop();
}

VideoWindow& PlaybackOutputController::window() {
  return impl_->presentation.window();
}

const VideoWindow& PlaybackOutputController::window() const {
  return impl_->presentation.window();
}

GpuVideoFrameCache& PlaybackOutputController::frameCache() {
  return impl_->presentation.frameCache();
}

void PlaybackOutputController::requestWindowPresent() {
  impl_->presentation.requestPresent();
}
