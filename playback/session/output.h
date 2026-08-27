#pragma once

#include <memory>
#include <string>

#include "core/native_wait_handle.h"
#include "playback/framebuffer/presenter.h"
#include "playback/session/presentation_policy.h"
#include "state.h"

class Player;
class GpuRuntime;
class VideoWindow;
class GpuVideoFrameCache;
struct InputEvent;
struct WindowUiState;

class PlaybackOutputController {
 public:
  PlaybackOutputController(
      Player& player, GpuRuntime& gpu, std::string mediaTitle,
      std::shared_ptr<playback_framebuffer_presenter::PresentationSource>
          presentationSource);
  ~PlaybackOutputController();

  PlaybackOutputController(PlaybackOutputController&&) noexcept;
  PlaybackOutputController& operator=(PlaybackOutputController&&) noexcept;

  PlaybackOutputController(const PlaybackOutputController&) = delete;
  PlaybackOutputController& operator=(const PlaybackOutputController&) = delete;

  bool windowOpen() const;
  bool windowVisible() const;
  bool consumeWindowCloseRequested();
  NativeWaitHandle windowInputWaitHandle() const;
  NativeWaitHandle windowCloseRequestedWaitHandle() const;
  bool openWindow();
  void closeWindow();

  bool pollWindowInput(InputEvent& event);
  void updateWindowCursor(Player& player, PlaybackSessionState playbackState,
                          bool overlayVisible);

  bool applyWindowPresentation(WindowPresentationRequest request);
  bool restoreWindowPresentation(
      WindowPresentationRequest request,
      const WindowPlacementState& placement);
  bool captureWindowPlacement(WindowPlacementState& placement);
  bool activateWindow();
  VideoWindow& window();
  const VideoWindow& window() const;
  GpuVideoFrameCache& frameCache();
  void requestWindowPresent();
  bool copyCurrentVideoFrameToClipboard(std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
