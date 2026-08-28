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

  const auto ignoredPause = window_input_events::translateAppCommand(
      appCommand(APPCOMMAND_MEDIA_PAUSE), SystemMediaInputPolicy::Ignore);
  const auto ignoredToggle = window_input_events::translateAppCommand(
      appCommand(APPCOMMAND_MEDIA_PLAY_PAUSE),
      SystemMediaInputPolicy::Ignore);
  const auto ignoredStop = window_input_events::translateAppCommand(
      appCommand(APPCOMMAND_MEDIA_STOP), SystemMediaInputPolicy::Ignore);
  ok &= expect(ignoredPause.handled && !ignoredPause.event,
               "an externally owned pause command must be consumed without "
               "entering window input");
  ok &= expect(ignoredToggle.handled && !ignoredToggle.event,
               "an externally owned toggle command must be consumed without "
               "entering window input");
  ok &= expect(ignoredStop.handled && !ignoredStop.event,
               "an externally owned stop command must be consumed without "
               "entering window input");

  const auto browserBack = window_input_events::translateAppCommand(
      appCommand(APPCOMMAND_BROWSER_BACKWARD),
      SystemMediaInputPolicy::Ignore);
  ok &= expect(browserBack.handled && browserBack.event &&
                   browserBack.event->type == InputEvent::Type::Action &&
                   browserBack.event->action == InputAction::Back,
               "browser commands must remain available under external media ownership");

  ok &= expect(isKey(window_input_events::translateAppCommand(
                         appCommand(APPCOMMAND_MEDIA_PAUSE),
                         SystemMediaInputPolicy::Translate).event,
                     kPlaybackVkMediaPause),
               "the window fallback must translate an explicit pause command");
  ok &= expect(isKey(window_input_events::translateAppCommand(
                         appCommand(APPCOMMAND_MEDIA_PLAY_PAUSE),
                         SystemMediaInputPolicy::Translate).event,
                     VK_MEDIA_PLAY_PAUSE),
               "the window fallback must translate a play/pause toggle");

  const auto unknown = window_input_events::translateAppCommand(
      appCommand(APPCOMMAND_VOLUME_MUTE), SystemMediaInputPolicy::Ignore);
  ok &= expect(!unknown.handled && !unknown.event,
               "unowned application commands must remain available to the "
               "default window procedure");

  return ok ? 0 : 1;
}
