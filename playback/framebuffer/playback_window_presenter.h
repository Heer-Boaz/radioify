#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include "gpu_shared.h"
#include "playback_framebuffer_presenter.h"
#include "playback_session_state.h"
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
             const playback_framebuffer_presenter::PictureInPictureTextGridProvider&
                 buildPictureInPictureTextGrid,
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
