#pragma once

#include <functional>
#include <memory>

#include "core/native_wait_handle.h"
#include "playback/video/gpu/gpu_shared.h"
#include "playback/framebuffer/presenter.h"
#include "playback_mode.h"
#include "state.h"
#include "playback/video/framebuffer/window/window.h"

class Player;

struct PlaybackPresenterSyncResult {
  PlaybackLayout previousActiveLayout = PlaybackLayout::Terminal;
  PlaybackLayout activeLayout = PlaybackLayout::Terminal;
  bool windowStartFailed = false;

  bool switchedAwayFromWindow() const {
    return previousActiveLayout == PlaybackLayout::Window &&
           activeLayout != PlaybackLayout::Window;
  }
};

class PlaybackPresentation {
 public:
  explicit PlaybackPresentation(PlaybackLayout initialLayout);
  ~PlaybackPresentation();

  PlaybackPresentation(PlaybackPresentation&&) noexcept;
  PlaybackPresentation& operator=(PlaybackPresentation&&) noexcept;

  PlaybackPresentation(const PlaybackPresentation&) = delete;
  PlaybackPresentation& operator=(const PlaybackPresentation&) = delete;

  bool windowRequested() const;
  bool windowActive() const;
  bool windowOpen() const;
  bool windowVisible() const;
  HWND nativeWindowHandle() const;
  bool consumeWindowCloseRequested();
  NativeWaitHandle windowCloseRequestedWaitHandle() const;
  PlaybackRenderMode renderMode(bool enableAscii) const;
  void requestLayout(PlaybackLayout layout);
  PlaybackLayout desiredLayout() const;

  PlaybackPresenterSyncResult sync(
      Player& player,
      const std::function<WindowUiState()>& buildUiState,
      const playback_framebuffer_presenter::TextGridPresentationProvider&
          buildTextGridPresentation,
      bool& redraw, bool& forceRefreshArt);

  void stop();

  bool applyWindowPresentation(PlaybackWindowPresentationRequest request);
  bool restoreWindowPresentation(
      PlaybackWindowPresentationRequest request,
      const WindowPlacementState& placement);
  bool captureWindowPlacement(
      WindowPlacementState& placement,
      const PlaybackPresentationState& presentation);
  bool activateWindow();
  void setWindowCursorVisible(bool visible);
  VideoWindow& window();
  const VideoWindow& window() const;
  void requestPresent();
  VideoFrameSnapshotResult captureCurrentFrame();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
