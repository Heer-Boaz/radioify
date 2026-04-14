#include "playback_session_output.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "consoleinput.h"
#include "core/windows_message_pump.h"
#include "player.h"
#include "playback/ascii/playback_screen_renderer.h"
#include "playback_session_presentation.h"

struct PlaybackOutputController::Impl {
  explicit Impl(PlaybackLayout initialLayout) : presentation(initialLayout) {}

  PlaybackPresentation presentation;
};

PlaybackOutputController::PlaybackOutputController(PlaybackLayout initialLayout)
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

bool PlaybackOutputController::windowVisible() const {
  return impl_->presentation.window().IsVisible();
}

PlaybackRenderMode PlaybackOutputController::renderMode(bool enableAscii) const {
  return impl_->presentation.renderMode(enableAscii);
}

void PlaybackOutputController::requestLayout(PlaybackLayout layout) {
  impl_->presentation.requestLayout(layout);
}

PlaybackLayout& PlaybackOutputController::desiredLayout() {
  return impl_->presentation.desiredLayout();
}

PlaybackPresenterSyncResult PlaybackOutputController::sync(
    Player& player,
    const std::function<WindowUiState()>& buildUiState,
    const std::function<bool()>& overlayVisible, bool& redraw,
    bool& forceRefreshArt, std::atomic<int64_t>& overlayUntilMs,
    std::atomic<int>& overlayControlHover) {
  return impl_->presentation.sync(player, buildUiState, overlayVisible, redraw,
                                  forceRefreshArt, overlayUntilMs,
                                  overlayControlHover);
}

void PlaybackOutputController::pollWindowEvents() {
  if (!impl_->presentation.window().IsOpen()) {
    return;
  }
  const bool wasVisible = impl_->presentation.window().IsVisible();
  const bool handledEvents = impl_->presentation.window().PollEvents();
  const bool isVisible = impl_->presentation.window().IsVisible();
  if (impl_->presentation.windowActive() &&
      ((handledEvents && isVisible) || (wasVisible != isVisible))) {
    impl_->presentation.requestPresent();
  }
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
  HANDLE handles[2];
  DWORD handleCount = 0;
  if (HANDLE inputHandle = input.waitHandle()) {
    handles[handleCount++] = inputHandle;
  }
  if (extraHandle) {
    handles[handleCount++] = extraHandle;
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
  impl_->presentation.frameCache().Reset();
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
