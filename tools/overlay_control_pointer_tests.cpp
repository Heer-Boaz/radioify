#include <cstdlib>
#include <iostream>

#include "playback/session/overlay_control_pointer.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "overlay_control_pointer_tests: " << message << '\n';
  return false;
}

MouseEvent pointer(MouseEventKind kind, bool down) {
  MouseEvent mouse;
  mouse.kind = kind;
  mouse.button = MouseButton::Left;
  mouse.buttons = down ? MouseButtons::Left : MouseButtons::None;
  return mouse;
}

}  // namespace

int main() {
  using Control = playback_overlay::OverlayControlId;
  using Surface = playback_video_timeline_preview::PresentationSurface;

  bool ok = true;
  playback_session_pointer::State state;

  auto interaction =
      state.handle(pointer(MouseEventKind::Press, true), Surface::VideoWindow,
                   Control::MediaTaskCancel);
  ok &= expect(interaction.captured && !interaction.activated,
               "press must arm without activating a playback control");
  interaction = state.handle(pointer(MouseEventKind::Release, false),
                             Surface::VideoWindow, Control::MediaTaskCancel);
  ok &= expect(
      interaction.captured && interaction.activated == Control::MediaTaskCancel,
      "release over the same control and surface must activate");

  state.handle(pointer(MouseEventKind::Press, true), Surface::VideoWindow,
               Control::MediaTaskCancel);
  interaction = state.handle(pointer(MouseEventKind::Release, false),
                             Surface::VideoWindow, std::nullopt);
  ok &= expect(interaction.captured && !interaction.activated,
               "dragging away before release must cancel the click");

  state.handle(pointer(MouseEventKind::Press, true), Surface::Terminal,
               Control::PlayPause);
  interaction = state.handle(pointer(MouseEventKind::Release, false),
                             Surface::VideoWindow, Control::PlayPause);
  ok &= expect(interaction.captured && !interaction.activated,
               "a release on another presentation surface must not activate");

  interaction = state.handle(pointer(MouseEventKind::Release, false),
                             Surface::Terminal, Control::PlayPause);
  ok &= expect(!interaction.captured && !interaction.activated,
               "release without an armed press must be inert");

  state.handle(pointer(MouseEventKind::Press, true), Surface::VideoWindow,
               Control::MediaTaskCancel);
  state.reset();
  interaction = state.handle(pointer(MouseEventKind::Release, false),
                             Surface::VideoWindow,
                             Control::MediaTaskCancel);
  ok &= expect(!interaction.captured && !interaction.activated,
               "resize-style pointer reset must prevent release activation");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
