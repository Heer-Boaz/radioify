#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/native_wait_handle.h"
#include "playback/framebuffer/presenter.h"
#include "playback/session/presentation_policy.h"
#include "state.h"

class Player;
class VideoWindow;
class GpuVideoFrameCache;
struct InputEvent;
struct WindowUiState;

namespace playback_screen_renderer {
struct PlaybackScreenRenderInputs;
}

class PlaybackOutputController {
 public:
  PlaybackOutputController();
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
  bool openWindow(
      Player& player,
      const std::function<WindowUiState()>& buildUiState,
      const playback_framebuffer_presenter::TextGridPresentationProvider&
          buildTextGridPresentation);
  void closeWindow();

  bool pollWindowInput(InputEvent& event);
  void updateWindowCursor(Player& player, PlaybackSessionState playbackState,
                          bool overlayVisible);
  void renderTerminal(
      playback_screen_renderer::PlaybackScreenRenderInputs& inputs);

  bool applyWindowPresentation(PlaybackWindowPresentationRequest request);
  bool restoreWindowPresentation(
      PlaybackWindowPresentationRequest request,
      const WindowPlacementState& placement);
  bool captureWindowPlacement(
      WindowPlacementState& placement,
      const PlaybackPresentationState& presentation);
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
