#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "playback/video/gpu/gpu_shared.h"
#include "playback/framebuffer/presenter.h"
#include "playback_mode.h"
#include "state.h"
#include "playback/video/framebuffer/window/window.h"

class Player;

struct PlaybackPresenterSyncResult {
  PlaybackLayout previousActiveLayout = PlaybackLayout::Terminal;
  PlaybackLayout activeLayout = PlaybackLayout::Terminal;

  bool switchedAwayFromWindow() const {
    return previousActiveLayout == PlaybackLayout::Window &&
           activeLayout != PlaybackLayout::Window;
  }
};

class PlaybackPresentation {
 public:
  explicit PlaybackPresentation(
      PlaybackLayout initialLayout,
      std::optional<PlaybackSessionContinuationState> initialState = std::nullopt);
  ~PlaybackPresentation();

  PlaybackPresentation(PlaybackPresentation&&) noexcept;
  PlaybackPresentation& operator=(PlaybackPresentation&&) noexcept;

  PlaybackPresentation(const PlaybackPresentation&) = delete;
  PlaybackPresentation& operator=(const PlaybackPresentation&) = delete;

  bool windowRequested() const;
  bool windowActive() const;
  bool consumeWindowCloseRequested();
  HANDLE windowCloseRequestedWaitHandle() const;
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

  void stop();

  VideoWindow& window();
  const VideoWindow& window() const;
  GpuVideoFrameCache& frameCache();
  void requestPresent();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
