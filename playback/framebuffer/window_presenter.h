#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include "gpu_shared.h"
#include "presenter.h"
#include "playback/session/state.h"
#include "player.h"
#include "videowindow.h"

class PlaybackWindowPresenter {
 public:
  PlaybackWindowPresenter();
  ~PlaybackWindowPresenter();

  PlaybackWindowPresenter(const PlaybackWindowPresenter&) = delete;
  PlaybackWindowPresenter& operator=(const PlaybackWindowPresenter&) = delete;

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
