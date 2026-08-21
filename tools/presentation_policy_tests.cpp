#include "playback/session/presentation_policy.h"

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

  const PlaybackPresentationState terminal =
      PlaybackPresentationState::terminalAscii();
  ok &= expect(terminal.usesAsciiGrid(),
               "terminal playback must use the ASCII grid");
  ok &= expect(terminal.baseSurface() ==
                   PlaybackBaseSurface::TerminalPlayback,
               "terminal playback must own the terminal base surface");
  ok &= expect(terminal.terminalRole() ==
                   PlaybackShellTerminalRole::Playback,
               "terminal playback must reserve the terminal for playback");
  ok &= expect(!windowPresentationRequest(
                    terminal,
                    PlaybackPresentationFocus::KeepCurrentSurface),
               "terminal playback must not request a native window");

  const PlaybackPresentationState windowed = terminal.toggleWindowMode();
  ok &= expect(windowed.visual() == PlaybackVisualMode::Framebuffer,
               "Ctrl+W must select framebuffer rendering");
  ok &= expect(windowed.baseSurface() ==
                   PlaybackBaseSurface::NativeWindowed,
               "Ctrl+W must select a normal native window");
  ok &= expect(windowed.terminalRole() == PlaybackShellTerminalRole::Browser,
               "native playback must release the terminal to the browser");
  const auto windowedRequest = windowPresentationRequest(
      windowed, PlaybackPresentationFocus::FocusTargetSurface);
  ok &= expect(windowedRequest &&
                   windowedRequest->target ==
                       PlaybackWindowPresentationMode::Windowed &&
                   windowedRequest->visual == PlaybackVisualMode::Framebuffer,
               "windowed framebuffer state must map to a concrete windowed request");

  const PlaybackPresentationState terminalRoundTrip =
      windowed.toggleWindowMode();
  ok &= expect(terminalRoundTrip == terminal,
               "Ctrl+W must round-trip between terminal ASCII and windowed framebuffer");

  const PlaybackPresentationState terminalFullscreen =
      terminal.toggleFullscreen();
  ok &= expect(terminalFullscreen.layer() ==
                   PlaybackPresentationLayer::Fullscreen &&
                   terminalFullscreen.usesAsciiGrid(),
               "Alt+Enter from terminal must open ASCII fullscreen");
  ok &= expect(terminalFullscreen.toggleFullscreen() == terminal,
               "Alt+Enter from ASCII fullscreen must return to terminal playback");

  const PlaybackPresentationState windowedFullscreen =
      windowed.toggleFullscreen();
  ok &= expect(windowedFullscreen.layer() ==
                   PlaybackPresentationLayer::Fullscreen,
               "Alt+Enter from a normal window must enter fullscreen");
  ok &= expect(windowedFullscreen.toggleFullscreen() == windowed,
               "Alt+Enter from framebuffer fullscreen must return to its normal window");
  ok &= expect(terminalFullscreen.toggleWindowMode() == windowedFullscreen &&
                   windowedFullscreen.toggleWindowMode() ==
                       terminalFullscreen,
               "Ctrl+W in fullscreen must only change the renderer and base surface");
  const auto terminalFullscreenRequest = windowPresentationRequest(
      terminalFullscreen, PlaybackPresentationFocus::FocusTargetSurface);
  ok &= expect(terminalFullscreenRequest &&
                   terminalFullscreenRequest->target ==
                       PlaybackWindowPresentationMode::Fullscreen &&
                   terminalFullscreenRequest->visual ==
                       PlaybackVisualMode::AsciiGrid,
               "ASCII fullscreen must map directly to one native fullscreen request");

  const PlaybackPresentationState terminalPip =
      terminal.togglePictureInPicture();
  ok &= expect(terminalPip.layer() ==
                   PlaybackPresentationLayer::PictureInPicture &&
                   terminalPip.pictureInPictureReturn() ==
                       PlaybackPictureInPictureReturn::Base,
               "PiP from terminal must remember the terminal base origin");
  ok &= expect(terminalPip.togglePictureInPicture() == terminal,
               "PiP from terminal must return exactly to terminal playback");
  ok &= expect(terminalPip.terminalRole() ==
                   PlaybackShellTerminalRole::Browser,
               "PiP must release the terminal to the browser");
  ok &= expect(presentationFocusFor(terminalPip) ==
                   PlaybackPresentationFocus::KeepCurrentSurface,
               "entering PiP must not steal focus before the browser is activated");
  ok &= expect(entersPictureInPicture(terminal, terminalPip),
               "the policy must identify the transition into PiP");
  ok &= expect(shellFocusAfterTransition(terminal, terminalPip) ==
                   PlaybackShellFocusTarget::Browser,
               "entering PiP must return focus ownership to the browser");
  ok &= expect(shellFocusAfterTransition(terminalPip, terminal) ==
                   PlaybackShellFocusTarget::TerminalPlayback,
               "returning from terminal-origin PiP must focus terminal playback");

  const PlaybackPresentationState fullscreenPip =
      windowedFullscreen.togglePictureInPicture();
  ok &= expect(fullscreenPip.pictureInPictureReturn() ==
                   PlaybackPictureInPictureReturn::Fullscreen,
               "PiP from fullscreen must remember fullscreen as its origin");
  ok &= expect(fullscreenPip.togglePictureInPicture() == windowedFullscreen,
               "PiP from fullscreen must return exactly to fullscreen");
  ok &= expect(presentationFocusFor(windowedFullscreen) ==
                   PlaybackPresentationFocus::FocusTargetSurface,
               "leaving PiP for fullscreen must focus the restored surface");
  ok &= expect(!entersPictureInPicture(fullscreenPip, windowedFullscreen),
               "leaving PiP must not be classified as entering PiP");
  ok &= expect(!shellFocusAfterTransition(fullscreenPip,
                                          windowedFullscreen),
               "native PiP return must leave focus to the native request");

  const PlaybackPresentationState windowedPip =
      windowed.togglePictureInPicture();
  ok &= expect(windowedPip.togglePictureInPicture() == windowed,
               "PiP from a normal window must return to that normal window");
  const auto windowedPipRequest = windowPresentationRequest(
      windowedPip, presentationFocusFor(windowedPip));
  ok &= expect(windowedPipRequest &&
                   windowedPipRequest->target ==
                       PlaybackWindowPresentationMode::PictureInPicture &&
                   windowedPipRequest->visual ==
                       PlaybackVisualMode::Framebuffer &&
                   windowedPipRequest->focus ==
                       PlaybackPresentationFocus::KeepCurrentSurface,
               "framebuffer PiP must map to one non-activating native request");

  const PlaybackPresentationState fullscreenFromPip =
      terminalPip.toggleFullscreen();
  ok &= expect(fullscreenFromPip.layer() ==
                   PlaybackPresentationLayer::Fullscreen &&
                   fullscreenFromPip.toggleFullscreen() == terminal,
               "Alt+Enter from PiP must replace PiP with fullscreen and then return to base");

  const PlaybackPresentationState framebufferPip =
      terminalPip.toggleWindowMode();
  ok &= expect(framebufferPip.visual() == PlaybackVisualMode::Framebuffer &&
                   framebufferPip.baseSurface() ==
                       PlaybackBaseSurface::NativeWindowed &&
                   framebufferPip.layer() ==
                       PlaybackPresentationLayer::PictureInPicture &&
                   framebufferPip.togglePictureInPicture() == windowed,
               "Ctrl+W in PiP must change the visual mode without losing the PiP layer");
  ok &= expect(presentationFocusFor(framebufferPip) ==
                   PlaybackPresentationFocus::KeepCurrentSurface,
               "changing the renderer inside PiP must preserve workspace focus");

  return ok ? 0 : 1;
}
