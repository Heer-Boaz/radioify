#pragma once

#include <optional>

enum class PlaybackVisualMode {
  AsciiGrid,
  Framebuffer,
};

enum class PlaybackBaseSurface {
  TerminalPlayback,
  NativeWindowed,
};

enum class PlaybackPresentationLayer {
  Base,
  Fullscreen,
  PictureInPicture,
};

enum class PlaybackPictureInPictureReturn {
  Base,
  Fullscreen,
};

enum class PlaybackShellTerminalRole {
  Browser,
  Playback,
};

enum class PlaybackPrimarySurface {
  Browser,
  TerminalPlayback,
  NativePlayback,
};

// User-visible presentation state. Construction is intentionally constrained:
// framebuffer playback can never target the terminal and PiP can only return
// to the base surface or to fullscreen.
class PlaybackPresentationState {
 public:
  constexpr PlaybackPresentationState() = default;

  static constexpr PlaybackPresentationState terminalAscii() {
    return PlaybackPresentationState(
        PlaybackVisualMode::AsciiGrid,
        PlaybackBaseSurface::TerminalPlayback,
        PlaybackPresentationLayer::Base,
        PlaybackPictureInPictureReturn::Base);
  }

  static constexpr PlaybackPresentationState nativeWindowed(
      PlaybackVisualMode visual = PlaybackVisualMode::Framebuffer) {
    return PlaybackPresentationState(
        visual, PlaybackBaseSurface::NativeWindowed,
        PlaybackPresentationLayer::Base,
        PlaybackPictureInPictureReturn::Base);
  }

  constexpr PlaybackVisualMode visual() const { return visual_; }
  constexpr PlaybackBaseSurface baseSurface() const { return baseSurface_; }
  constexpr PlaybackPresentationLayer layer() const { return layer_; }
  constexpr PlaybackPictureInPictureReturn pictureInPictureReturn() const {
    return pictureInPictureReturn_;
  }

  constexpr bool usesAsciiGrid() const {
    return visual_ == PlaybackVisualMode::AsciiGrid;
  }

  constexpr bool requiresNativeWindow() const {
    return layer_ != PlaybackPresentationLayer::Base ||
           baseSurface_ == PlaybackBaseSurface::NativeWindowed;
  }

  constexpr PlaybackShellTerminalRole terminalRole() const {
    return requiresNativeWindow() ? PlaybackShellTerminalRole::Browser
                                  : PlaybackShellTerminalRole::Playback;
  }

  constexpr PlaybackPrimarySurface primarySurface() const {
    if (layer_ == PlaybackPresentationLayer::PictureInPicture) {
      return PlaybackPrimarySurface::Browser;
    }
    return requiresNativeWindow() ? PlaybackPrimarySurface::NativePlayback
                                  : PlaybackPrimarySurface::TerminalPlayback;
  }

  constexpr PlaybackPresentationState toggleWindowMode() const {
    PlaybackPresentationState next = *this;
    if (visual_ == PlaybackVisualMode::AsciiGrid) {
      next.visual_ = PlaybackVisualMode::Framebuffer;
      next.baseSurface_ = PlaybackBaseSurface::NativeWindowed;
    } else {
      next.visual_ = PlaybackVisualMode::AsciiGrid;
      next.baseSurface_ = PlaybackBaseSurface::TerminalPlayback;
    }
    return next;
  }

  constexpr PlaybackPresentationState toggleFullscreen() const {
    PlaybackPresentationState next = *this;
    next.pictureInPictureReturn_ =
        PlaybackPictureInPictureReturn::Base;
    next.layer_ = layer_ == PlaybackPresentationLayer::Fullscreen
                      ? PlaybackPresentationLayer::Base
                      : PlaybackPresentationLayer::Fullscreen;
    return next;
  }

  constexpr PlaybackPresentationState togglePictureInPicture() const {
    PlaybackPresentationState next = *this;
    if (layer_ == PlaybackPresentationLayer::PictureInPicture) {
      next.layer_ =
          pictureInPictureReturn_ ==
                  PlaybackPictureInPictureReturn::Fullscreen
              ? PlaybackPresentationLayer::Fullscreen
              : PlaybackPresentationLayer::Base;
      next.pictureInPictureReturn_ =
          PlaybackPictureInPictureReturn::Base;
      return next;
    }

    next.pictureInPictureReturn_ =
        layer_ == PlaybackPresentationLayer::Fullscreen
            ? PlaybackPictureInPictureReturn::Fullscreen
            : PlaybackPictureInPictureReturn::Base;
    next.layer_ = PlaybackPresentationLayer::PictureInPicture;
    return next;
  }

  friend constexpr bool operator==(const PlaybackPresentationState& lhs,
                                   const PlaybackPresentationState& rhs) {
    return lhs.visual_ == rhs.visual_ &&
           lhs.baseSurface_ == rhs.baseSurface_ &&
           lhs.layer_ == rhs.layer_ &&
           lhs.pictureInPictureReturn_ == rhs.pictureInPictureReturn_;
  }

  friend constexpr bool operator!=(const PlaybackPresentationState& lhs,
                                   const PlaybackPresentationState& rhs) {
    return !(lhs == rhs);
  }

 private:
  constexpr PlaybackPresentationState(
      PlaybackVisualMode visual, PlaybackBaseSurface baseSurface,
      PlaybackPresentationLayer layer,
      PlaybackPictureInPictureReturn pictureInPictureReturn)
      : visual_(visual),
        baseSurface_(baseSurface),
        layer_(layer),
        pictureInPictureReturn_(pictureInPictureReturn) {}

  PlaybackVisualMode visual_ = PlaybackVisualMode::AsciiGrid;
  PlaybackBaseSurface baseSurface_ =
      PlaybackBaseSurface::TerminalPlayback;
  PlaybackPresentationLayer layer_ = PlaybackPresentationLayer::Base;
  PlaybackPictureInPictureReturn pictureInPictureReturn_ =
      PlaybackPictureInPictureReturn::Base;
};

enum class PlaybackWindowPresentationMode {
  Windowed,
  Fullscreen,
  PictureInPicture,
};

enum class PlaybackPresentationFocus {
  KeepCurrentSurface,
  FocusTargetSurface,
};

enum class PlaybackShellFocusTarget {
  Browser,
  TerminalPlayback,
};

inline constexpr PlaybackPresentationFocus presentationFocusFor(
    const PlaybackPresentationState& target) {
  return target.layer() == PlaybackPresentationLayer::PictureInPicture
             ? PlaybackPresentationFocus::KeepCurrentSurface
             : PlaybackPresentationFocus::FocusTargetSurface;
}

inline constexpr bool entersPictureInPicture(
    const PlaybackPresentationState& previous,
    const PlaybackPresentationState& target) {
  return previous.layer() != PlaybackPresentationLayer::PictureInPicture &&
         target.layer() == PlaybackPresentationLayer::PictureInPicture;
}

inline constexpr std::optional<PlaybackShellFocusTarget>
shellFocusAfterTransition(const PlaybackPresentationState& previous,
                          const PlaybackPresentationState& target) {
  if (entersPictureInPicture(previous, target)) {
    return PlaybackShellFocusTarget::Browser;
  }
  if (previous.requiresNativeWindow() && !target.requiresNativeWindow()) {
    return PlaybackShellFocusTarget::TerminalPlayback;
  }
  return std::nullopt;
}

struct PlaybackWindowPresentationRequest {
  PlaybackWindowPresentationMode target =
      PlaybackWindowPresentationMode::Windowed;
  PlaybackVisualMode visual = PlaybackVisualMode::Framebuffer;
  PlaybackPresentationFocus focus =
      PlaybackPresentationFocus::KeepCurrentSurface;
};

inline constexpr std::optional<PlaybackWindowPresentationRequest>
windowPresentationRequest(const PlaybackPresentationState& state,
                          PlaybackPresentationFocus focus) {
  if (!state.requiresNativeWindow()) {
    return std::nullopt;
  }

  PlaybackWindowPresentationMode target =
      PlaybackWindowPresentationMode::Windowed;
  switch (state.layer()) {
    case PlaybackPresentationLayer::Base:
      break;
    case PlaybackPresentationLayer::Fullscreen:
      target = PlaybackWindowPresentationMode::Fullscreen;
      break;
    case PlaybackPresentationLayer::PictureInPicture:
      target = PlaybackWindowPresentationMode::PictureInPicture;
      break;
  }
  return PlaybackWindowPresentationRequest{target, state.visual(), focus};
}
