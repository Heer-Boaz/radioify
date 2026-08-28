#include "playback/session/overlay_control_pointer.h"

namespace playback_session_pointer {

Interaction State::handle(
    const MouseEvent& mouse,
    playback_video_timeline_preview::PresentationSurface surface,
    std::optional<playback_overlay::OverlayControlId> hovered) {
  Interaction result;
  const std::optional<Target> target =
      hovered ? std::optional<Target>(Target{surface, *hovered}) : std::nullopt;

  if (mouse.kind == MouseEventKind::Press &&
      mouse.button == MouseButton::Left &&
      isMouseButtonDown(mouse, MouseButton::Left)) {
    armed_ = target;
    result.captured = armed_.has_value();
    return result;
  }

  if (mouse.kind == MouseEventKind::Release &&
      mouse.button == MouseButton::Left) {
    result.captured = armed_.has_value();
    if (armed_ && target == armed_) {
      result.activated = armed_->control;
    }
    armed_.reset();
    return result;
  }

  result.captured = armed_.has_value();
  return result;
}

}  // namespace playback_session_pointer
