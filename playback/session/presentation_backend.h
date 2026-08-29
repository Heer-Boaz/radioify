#pragma once

#include <cstdint>

#include "playback/framebuffer/window_presentation.h"

enum class PlaybackWindowLifecycle : std::uint8_t {
  Closed,
  Opening,
  Open,
  Closing,
  Failed,
};

// Narrow owner-thread boundary used by the presentation state machine. Window
// creation and destruction are asynchronous; small window commands execute on
// the presenter thread only after the lifecycle reports Open.
class PlaybackPresentationBackend {
 public:
  virtual ~PlaybackPresentationBackend() = default;

  virtual PlaybackWindowLifecycle windowLifecycle() const = 0;
  virtual bool consumeWindowLifecycleChange() = 0;
  virtual bool requestOpenWindow() = 0;
  virtual void requestCloseWindow() = 0;
  virtual bool windowCloseReady() const = 0;
  virtual bool finishCloseWindow() = 0;

  virtual bool applyWindowPresentation(WindowPresentationRequest request) = 0;
  virtual bool
  restoreWindowPresentation(WindowPresentationRequest request,
                            const WindowPlacementState& placement) = 0;
  virtual bool captureWindowPlacement(WindowPlacementState& placement) = 0;
};
