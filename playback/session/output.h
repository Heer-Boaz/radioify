#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <memory>

#include "core/native_wait_handle.h"
#include "playback/framebuffer/presenter.h"
#include "playback_mode.h"
#include "state.h"

class ConsoleInput;
class Player;
class VideoWindow;
class GpuVideoFrameCache;
struct InputEvent;
struct WindowUiState;

namespace playback_screen_renderer {
struct PlaybackScreenRenderInputs;
}

struct PlaybackPresenterSyncResult;

class PlaybackOutputController {
 public:
  explicit PlaybackOutputController(
      PlaybackLayout initialLayout,
      std::optional<PlaybackSessionContinuationState> initialState =
          std::nullopt);
  ~PlaybackOutputController();

  PlaybackOutputController(PlaybackOutputController&&) noexcept;
  PlaybackOutputController& operator=(PlaybackOutputController&&) noexcept;

  PlaybackOutputController(const PlaybackOutputController&) = delete;
  PlaybackOutputController& operator=(const PlaybackOutputController&) = delete;

  bool windowRequested() const;
  bool windowActive() const;
  bool windowVisible() const;
  bool consumeWindowCloseRequested();
  PlaybackRenderMode renderMode(bool enableAscii) const;
  void requestLayout(PlaybackLayout layout);
  PlaybackLayout desiredLayout() const;

  PlaybackPresenterSyncResult sync(
      Player& player,
      const std::function<WindowUiState()>& buildUiState,
      const std::function<bool()>& overlayVisible,
      const playback_framebuffer_presenter::TextGridPresentationProvider&
          buildTextGridPresentation,
      bool& redraw,
      bool& forceRefreshArt, std::atomic<int64_t>& overlayUntilMs,
      std::atomic<int>& overlayControlHover);

  bool pollInput(ConsoleInput& input, InputEvent& ev);
  bool waitForActivity(ConsoleInput& input, int timeoutMs,
                       NativeWaitHandle extraHandle = NativeWaitHandle(),
                       NativeWaitHandle secondExtraHandle = NativeWaitHandle());
  void updateWindowCursor(Player& player, PlaybackSessionState playbackState,
                          bool overlayVisible);
  void renderTerminal(
      playback_screen_renderer::PlaybackScreenRenderInputs& inputs);

  void stop();

  VideoWindow& window();
  const VideoWindow& window() const;
  GpuVideoFrameCache& frameCache();
  void requestWindowPresent();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
