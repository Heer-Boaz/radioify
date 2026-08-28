#include <cstdint>
#include <cstdio>

#include <windows.h>

#include "input_event.h"
#include "playback/input/media_keys.h"
#include "playback/video/framebuffer/window/input_events.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::fprintf(stderr, "window_input_events_tests: %s\n", message);
  return false;
}

LPARAM appCommand(int command) {
  return static_cast<LPARAM>(static_cast<std::uint32_t>(command) << 16);
}

bool isKey(const std::optional<InputEvent>& event, WORD key) {
  return event && event->type == InputEvent::Type::Key &&
         event->key.vk == key;
}

}  // namespace

int main() {
  using window_input_events::SystemMediaInputPolicy;

  bool ok = true;
  ok &= expect(window_input_events::isSystemMediaVirtualKey(
                   VK_MEDIA_PLAY_PAUSE),
               "play/pause must be classified as a system media key");
  ok &= expect(!window_input_events::isSystemMediaVirtualKey('W'),
               "ordinary window shortcuts must not be classified as media keys");

  ok &= expect(!window_input_events::inputEventFromAppCommand(
                   appCommand(APPCOMMAND_MEDIA_PAUSE),
                   SystemMediaInputPolicy::Ignore),
               "an externally owned pause command must not enter window input");
  ok &= expect(!window_input_events::inputEventFromAppCommand(
                   appCommand(APPCOMMAND_MEDIA_PLAY_PAUSE),
                   SystemMediaInputPolicy::Ignore),
               "an externally owned toggle command must not enter window input");
  ok &= expect(!window_input_events::inputEventFromAppCommand(
                   appCommand(APPCOMMAND_MEDIA_STOP),
                   SystemMediaInputPolicy::Ignore),
               "an externally owned stop command must not enter window input");

  const auto browserBack = window_input_events::inputEventFromAppCommand(
      appCommand(APPCOMMAND_BROWSER_BACKWARD),
      SystemMediaInputPolicy::Ignore);
  ok &= expect(browserBack && browserBack->type == InputEvent::Type::Action &&
                   browserBack->action == InputAction::Back,
               "browser commands must remain available under external media ownership");

  ok &= expect(isKey(window_input_events::inputEventFromAppCommand(
                         appCommand(APPCOMMAND_MEDIA_PAUSE),
                         SystemMediaInputPolicy::Translate),
                     kPlaybackVkMediaPause),
               "the window fallback must translate an explicit pause command");
  ok &= expect(isKey(window_input_events::inputEventFromAppCommand(
                         appCommand(APPCOMMAND_MEDIA_PLAY_PAUSE),
                         SystemMediaInputPolicy::Translate),
                     VK_MEDIA_PLAY_PAUSE),
               "the window fallback must translate a play/pause toggle");

  return ok ? 0 : 1;
}
