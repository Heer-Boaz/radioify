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

  const PlaybackPresentationState terminalPip =
      terminal.togglePictureInPicture();
  ok &= expect(terminalPip.layer() ==
                   PlaybackPresentationLayer::PictureInPicture &&
                   terminalPip.pictureInPictureReturn() ==
                       PlaybackPictureInPictureReturn::Base,
               "PiP from terminal must remember the terminal base origin");
  ok &= expect(terminalPip.togglePictureInPicture() == terminal,
               "PiP from terminal must return exactly to terminal playback");

  const PlaybackPresentationState fullscreenPip =
      windowedFullscreen.togglePictureInPicture();
  ok &= expect(fullscreenPip.pictureInPictureReturn() ==
                   PlaybackPictureInPictureReturn::Fullscreen,
               "PiP from fullscreen must remember fullscreen as its origin");
  ok &= expect(fullscreenPip.togglePictureInPicture() == windowedFullscreen,
               "PiP from fullscreen must return exactly to fullscreen");

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

  return ok ? 0 : 1;
}
