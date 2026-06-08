#pragma once

enum class PlaybackPresentationFamily {
  Ascii,
  Framebuffer,
};

enum class PlaybackPresentationMode {
  DefaultNonFullscreen,
  Fullscreen,
  PictureInPicture,
};

enum class PlaybackPresentationFocus {
  KeepCurrentSurface,
  FocusTargetSurface,
};

struct PlaybackWindowPresentationRequest {
  PlaybackPresentationMode target = PlaybackPresentationMode::Fullscreen;
  bool textGrid = false;
  PlaybackPresentationFocus focus =
      PlaybackPresentationFocus::KeepCurrentSurface;
};

inline PlaybackWindowPresentationRequest userRequestedWindowPresentation(
    PlaybackPresentationMode target, bool textGrid) {
  return {target, textGrid, PlaybackPresentationFocus::FocusTargetSurface};
}

inline PlaybackWindowPresentationRequest restoredWindowPresentation(
    PlaybackPresentationMode target, bool textGrid) {
  return {target, textGrid, PlaybackPresentationFocus::KeepCurrentSurface};
}

struct PlaybackFullscreenToggleRequest {
  PlaybackPresentationFamily family = PlaybackPresentationFamily::Ascii;
  PlaybackPresentationMode current =
      PlaybackPresentationMode::DefaultNonFullscreen;
};

struct PlaybackFullscreenTogglePlan {
  PlaybackPresentationMode target = PlaybackPresentationMode::Fullscreen;
};

inline PlaybackPresentationMode defaultNonFullscreenPresentation(
    PlaybackPresentationFamily family) {
  switch (family) {
    case PlaybackPresentationFamily::Ascii:
      return PlaybackPresentationMode::DefaultNonFullscreen;
    case PlaybackPresentationFamily::Framebuffer:
      return PlaybackPresentationMode::PictureInPicture;
  }
  return PlaybackPresentationMode::DefaultNonFullscreen;
}

inline PlaybackFullscreenTogglePlan planFullscreenToggle(
    PlaybackFullscreenToggleRequest request) {
  PlaybackFullscreenTogglePlan plan;
  plan.target = request.current == PlaybackPresentationMode::Fullscreen
                    ? defaultNonFullscreenPresentation(request.family)
                    : PlaybackPresentationMode::Fullscreen;
  return plan;
}
