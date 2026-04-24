#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include "playback/video/gpu/gpu_shared.h"
#include "presenter.h"
#include "playback/session/state.h"
#include "playback/video/player.h"
#include "playback/video/framebuffer/window/window.h"

class WindowPresenter {
 public:
  WindowPresenter();
  ~WindowPresenter();

  WindowPresenter(const WindowPresenter&) = delete;
  WindowPresenter& operator=(const WindowPresenter&) = delete;

  bool start(Player& player, const std::function<WindowUiState()>& buildUiState,
             const std::function<bool()>& overlayVisible,
             const playback_framebuffer_presenter::TextGridPresentationProvider&
                 buildTextGridPresentation,
             const PlaybackSessionContinuationState* initialState = nullptr);
  void stop();
  void requestPresent();

  bool isOpen() const;
  bool isVisible() const;
  bool consumeCloseRequested();
  HANDLE closeRequestedWaitHandle() const;

  VideoWindow& window();
  const VideoWindow& window() const;
  GpuVideoFrameCache& frameCache();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
