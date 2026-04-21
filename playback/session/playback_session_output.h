#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <memory>

#include "playback_framebuffer_presenter.h"
#include "playback_mode.h"
#include "playback_session_state.h"

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
  PlaybackLayout& desiredLayout();

  PlaybackPresenterSyncResult sync(
      Player& player,
      const std::function<WindowUiState()>& buildUiState,
      const std::function<bool()>& overlayVisible,
      const playback_framebuffer_presenter::PictureInPictureTextGridProvider&
          buildPictureInPictureTextGrid,
      bool& redraw,
      bool& forceRefreshArt, std::atomic<int64_t>& overlayUntilMs,
      std::atomic<int>& overlayControlHover);

  bool pollInput(ConsoleInput& input, InputEvent& ev);
  bool waitForActivity(ConsoleInput& input, int timeoutMs,
                       HANDLE extraHandle = nullptr);
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
