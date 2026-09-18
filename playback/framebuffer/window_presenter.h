#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "core/native_wait_handle.h"
#include "playback/framebuffer/window_presentation.h"
#include "playback/video/playback.h"
#include "playback/video/gpu/gpu_runtime.h"
#include "presenter.h"
#include "playback/video/player.h"
#include "playback/video/framebuffer/window/window.h"

class WindowPresenter {
 public:
  enum class Lifecycle : std::uint8_t {
    Closed,
    Opening,
    Open,
    Closing,
    Failed,
  };

  WindowPresenter(
      Player& player, GpuRuntime& gpu, std::string mediaTitle,
      std::shared_ptr<playback_framebuffer_presenter::PresentationSource>
          presentationSource);
  ~WindowPresenter();

  WindowPresenter(const WindowPresenter&) = delete;
  WindowPresenter& operator=(const WindowPresenter&) = delete;

  bool requestStart();
  Lifecycle lifecycle() const;
  bool consumeLifecycleChange();
  NativeWaitHandle lifecycleWaitHandle() const;
  void requestStop();
  bool stopReady() const;
  bool finishStop();
  NativeWaitHandle stopWaitHandle() const;
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
