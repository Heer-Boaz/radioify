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
