#include "playback/session/presentation_policy.h"
#include "playback/overlay/transient_message.h"

#include <chrono>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "presentation_policy_tests: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;

  const PlaybackWindowPresentationRequest defaultRequest;
  ok &= expect(defaultRequest.target == PlaybackPresentationMode::Fullscreen,
               "default window presentation target must be fullscreen");
  ok &= expect(!defaultRequest.textGrid,
               "default window presentation must not request text-grid mode");
  ok &= expect(defaultRequest.focus ==
                   PlaybackPresentationFocus::KeepCurrentSurface,
               "default window presentation must preserve current focus");

  const PlaybackWindowPresentationRequest pipFromTerminal =
      userRequestedWindowPresentation(
          PlaybackPresentationMode::PictureInPicture, true);
  ok &= expect(pipFromTerminal.target ==
                   PlaybackPresentationMode::PictureInPicture,
               "user-requested terminal PiP must target PiP");
  ok &= expect(pipFromTerminal.textGrid,
               "user-requested terminal PiP must carry text-grid mode");
  ok &= expect(pipFromTerminal.focus ==
                   PlaybackPresentationFocus::FocusTargetSurface,
               "user-requested PiP must focus the target surface");

  const PlaybackFullscreenTogglePlan framebufferFullscreenToggle =
      planFullscreenToggle({PlaybackPresentationFamily::Framebuffer,
                            PlaybackPresentationMode::Fullscreen});
  const PlaybackWindowPresentationRequest altEnterToPiP =
      userRequestedWindowPresentation(framebufferFullscreenToggle.target,
                                      false);
  ok &= expect(altEnterToPiP.target ==
                   PlaybackPresentationMode::PictureInPicture,
               "Alt+Enter from framebuffer fullscreen must target PiP");
  ok &= expect(altEnterToPiP.focus ==
                   PlaybackPresentationFocus::FocusTargetSurface,
               "Alt+Enter presentation changes must focus the target surface");

  const PlaybackWindowPresentationRequest restoredFullscreen =
      restoredWindowPresentation(PlaybackPresentationMode::Fullscreen, true);
  ok &= expect(restoredFullscreen.target ==
                   PlaybackPresentationMode::Fullscreen,
               "restored presentation must keep its target");
  ok &= expect(restoredFullscreen.textGrid,
               "restored presentation must preserve text-grid mode");
  ok &= expect(restoredFullscreen.focus ==
                   PlaybackPresentationFocus::KeepCurrentSurface,
               "restored presentation must not steal foreground focus");

  using namespace std::chrono_literals;
  playback_overlay::TransientMessage transientMessage;
  const playback_overlay::TransientMessage::TimePoint shownAt{100ms};
  transientMessage.show("Frame copied to clipboard", shownAt, 1500ms);
  ok &= expect(transientMessage.visibleAt(shownAt),
               "a transient message must be visible when published");
  ok &= expect(transientMessage.textAt(shownAt + 1499ms) ==
                   "Frame copied to clipboard",
               "a transient message must retain its text before expiry");
  ok &= expect(!transientMessage.visibleAt(shownAt + 1500ms),
               "a transient message must expire at its deadline");
  ok &= expect(!transientMessage.textAt(shownAt + 1500ms).has_value(),
               "expired transient text must not enter presentation state");

  transientMessage.show("Frame copy failed", shownAt + 250ms, 1500ms);
  ok &= expect(transientMessage.textAt(shownAt + 1500ms) ==
                   "Frame copy failed",
               "a newer transient message must replace and extend the slot");

  return ok ? 0 : 1;
}
