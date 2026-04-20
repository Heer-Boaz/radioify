#include "playback/playback_shortcuts.h"

#include <iostream>

namespace {

KeyEvent makeKey(WORD vk, char ch = 0, DWORD control = 0) {
  KeyEvent key{};
  key.vk = vk;
  key.ch = ch;
  key.control = control;
  return key;
}

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "shortcut_match_tests: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;

  ok &= expect(!matchesShortcut(makeKey(VK_F1), VK_ESCAPE, 0, 0),
               "VK_F1 must not match VK_ESCAPE when no ASCII fallback exists");
  ok &= expect(!matchesShortcut(makeKey(VK_F1), VK_BACK, 0, 0),
               "VK_F1 must not match VK_BACK when no ASCII fallback exists");
  ok &= expect(!resolvePlaybackShortcutAction(
                   makeKey(VK_F1), kPlaybackShortcutContextPlaybackSession)
                   .has_value(),
               "VK_F1 must not resolve to any playback-session shortcut");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_ESCAPE), kPlaybackShortcutContextPlaybackSession)
                   .value() == PlaybackShortcutAction::ExitPlaybackSession,
               "VK_ESCAPE must still exit playback session");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_ESCAPE), kPlaybackShortcutContextImageViewer)
                   .value() == PlaybackShortcutAction::CloseViewer,
               "VK_ESCAPE must still close the image viewer");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_LEFT), kPlaybackShortcutContextShared)
                   .value() == PlaybackShortcutAction::SeekBackward,
               "VK_LEFT must seek backward");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_RIGHT), kPlaybackShortcutContextShared)
                   .value() == PlaybackShortcutAction::SeekForward,
               "VK_RIGHT must seek forward");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_LEFT),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextImageViewer)
                   .value() == PlaybackShortcutAction::SeekBackward,
               "Image viewer must inherit the shared bare-arrow seek layer");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_RIGHT),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextImageViewer)
                   .value() == PlaybackShortcutAction::SeekForward,
               "Image viewer must inherit the shared bare-arrow seek layer");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_LEFT, 0, kPlaybackShortcutCtrlMask),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextImageViewer)
                   .value() == PlaybackShortcutAction::Previous,
               "Ctrl+VK_LEFT must navigate to the previous item");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_RIGHT, 0, kPlaybackShortcutCtrlMask),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextImageViewer)
                   .value() == PlaybackShortcutAction::Next,
               "Ctrl+VK_RIGHT must navigate to the next item");

  return ok ? 0 : 1;
}
