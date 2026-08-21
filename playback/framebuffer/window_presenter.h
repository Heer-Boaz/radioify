#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "core/native_wait_handle.h"
#include "playback/framebuffer/window_presentation.h"
#include "playback/video/gpu/gpu_shared.h"
#include "presenter.h"
#include "playback/video/player.h"
#include "playback/video/framebuffer/window/window.h"

class WindowPresenter {
 public:
  WindowPresenter(
      Player& player, std::string mediaTitle,
      std::function<WindowUiState()> buildUiState,
      playback_framebuffer_presenter::TextGridPresentationProvider
          buildTextGridPresentation);
  ~WindowPresenter();

  WindowPresenter(const WindowPresenter&) = delete;
  WindowPresenter& operator=(const WindowPresenter&) = delete;

  bool start();
  void stop();
  void requestPresent();
  bool applyPresentation(WindowPresentationRequest request);
  bool restorePresentation(WindowPresentationRequest request,
                           const WindowPlacementState& placement);
  bool capturePlacement(WindowPlacementState& placement);
  bool activate();
  void setCursorVisible(bool visible);
  VideoFrameSnapshotResult captureCurrentFrame();

  bool isOpen() const;
  bool isVisible() const;
  HWND nativeWindowHandle() const;
  bool consumeCloseRequested();
  NativeWaitHandle closeRequestedWaitHandle() const;

  VideoWindow& window();
  const VideoWindow& window() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
