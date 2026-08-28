#pragma once

#include <optional>

#include "playback/overlay/interaction.h"
#include "playback/video/timeline_preview_types.h"
#include "tui/input_event.h"

namespace playback_session_pointer {

struct Target {
  playback_video_timeline_preview::PresentationSurface surface =
      playback_video_timeline_preview::PresentationSurface::Terminal;
  playback_overlay::OverlayControlId control =
      playback_overlay::OverlayControlId::Radio;

  friend bool operator==(const Target& left, const Target& right) {
    return left.surface == right.surface && left.control == right.control;
  }
};

struct Interaction {
  bool captured = false;
  std::optional<playback_overlay::OverlayControlId> activated;
};

// Shared desktop-button pointer contract for ASCII and framebuffer playback:
// press arms one exact control and surface; release over that same target
// activates it. Moving away or leaving the surface makes the click reversible.
class State {
 public:
  Interaction handle(
      const MouseEvent& mouse,
      playback_video_timeline_preview::PresentationSurface surface,
      std::optional<playback_overlay::OverlayControlId> hovered);
  void reset() { armed_.reset(); }

 private:
  std::optional<Target> armed_;
};

}  // namespace playback_session_pointer
