#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

class VideoWindow;

enum class WindowPresentationMode {
  Windowed,
  Fullscreen,
  PictureInPicture,
};

enum class WindowContentMode {
  Framebuffer,
  TextGrid,
};

enum class WindowFocusPolicy {
  PreserveCurrent,
  ActivateWindow,
};

struct WindowPresentationRequest {
  WindowPresentationMode mode = WindowPresentationMode::Windowed;
  WindowContentMode content = WindowContentMode::Framebuffer;
  WindowFocusPolicy focus = WindowFocusPolicy::PreserveCurrent;
};

struct WindowPlacementState {
  bool hasWindowedRect = false;
  RECT windowedRect{};
  bool hasPictureInPictureRect = false;
  RECT pictureInPictureRect{};
};

namespace playback_window_presentation {

bool apply(VideoWindow& window, WindowPresentationRequest request);
void applyPlacement(VideoWindow& window, const WindowPlacementState& state);
void capturePlacement(const VideoWindow& window, WindowPlacementState& state);

}  // namespace playback_window_presentation
