#include <cstdint>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "input_event.h"
#include "playback/input/media_keys.h"
#include "playback/input/shortcuts.h"
#include "playback/video/framebuffer/window/input_events.h"
#include "tui/audio_picture_in_picture_window.h"

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

void appendPlaybackAction(const std::optional<InputEvent>& event,
                          std::vector<PlaybackAction>& actions) {
  if (!event || event->type != InputEvent::Type::Key) return;
  if (const auto action = resolvePlaybackAction(event->key)) {
    actions.push_back(*action);
  }
}

std::vector<PlaybackAction> routeNativeMediaGesture(
    SystemMediaCommandOwner owner, WORD virtualKey, int appCommandId) {
  std::vector<PlaybackAction> actions;
  switch (window_input_events::routeKeyDown(virtualKey, owner)) {
    case window_input_events::KeyDownRoute::Queue:
      appendPlaybackAction(
          window_input_events::keyFromVirtualKey(virtualKey), actions);
      break;
    case window_input_events::KeyDownRoute::DelegateToDefaultWindowProcedure:
      // DefWindowProc emits this second message for an application-command
      // key. Simulating both messages catches duplicate ingress.
      appendPlaybackAction(
          window_input_events::translateAppCommand(
              appCommand(appCommandId), owner)
              .event,
          actions);
      break;
    case window_input_events::KeyDownRoute::Consume:
      // Windows may still deliver a queued app-command message after the
      // owner switches; the external-owner route must consume that as well.
      appendPlaybackAction(
          window_input_events::translateAppCommand(
              appCommand(appCommandId), owner)
              .event,
          actions);
      break;
  }
  return actions;
}

std::optional<PlaybackAction> routeConsoleMediaKey(
    SystemMediaCommandOwner owner, WORD virtualKey) {
  if (!shouldDispatchLocalVirtualKey(virtualKey, owner)) {
    return std::nullopt;
  }
  return resolvePlaybackAction(
      window_input_events::keyFromVirtualKey(virtualKey).key);
}

}  // namespace

int main() {
  static_assert(
      !std::is_constructible_v<VideoWindow, GpuRuntime&> &&
          std::is_constructible_v<VideoWindow, GpuRuntime&,
                                  SystemMediaCommandOwner>,
      "every native input surface must declare the process media-command "
      "owner");
  static_assert(
      !std::is_constructible_v<AudioPictureInPictureWindow, GpuRuntime&> &&
          std::is_constructible_v<AudioPictureInPictureWindow, GpuRuntime&,
                                  SystemMediaCommandOwner>,
      "audio picture-in-picture must share the process media-command owner");

  bool ok = true;
  ok &= expect(isSystemMediaVirtualKey(VK_MEDIA_PLAY_PAUSE),
               "play/pause must be classified as a system media key");
  ok &= expect(!isSystemMediaVirtualKey('W'),
               "ordinary window shortcuts must not be classified as media keys");

  const auto ignoredPause = window_input_events::translateAppCommand(
      appCommand(APPCOMMAND_MEDIA_PAUSE),
      SystemMediaCommandOwner::SystemMediaTransportControls);
  const auto ignoredToggle = window_input_events::translateAppCommand(
      appCommand(APPCOMMAND_MEDIA_PLAY_PAUSE),
      SystemMediaCommandOwner::SystemMediaTransportControls);
  const auto ignoredStop = window_input_events::translateAppCommand(
      appCommand(APPCOMMAND_MEDIA_STOP),
      SystemMediaCommandOwner::SystemMediaTransportControls);
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
      SystemMediaCommandOwner::SystemMediaTransportControls);
  ok &= expect(browserBack.handled && browserBack.event &&
                   browserBack.event->type == InputEvent::Type::Action &&
                   browserBack.event->action == InputAction::Back,
               "browser commands must remain available under external media ownership");

  ok &= expect(isKey(window_input_events::translateAppCommand(
                         appCommand(APPCOMMAND_MEDIA_PAUSE),
                         SystemMediaCommandOwner::LocalInputFallback).event,
                     kPlaybackVkMediaPause),
               "the window fallback must translate an explicit pause command");
  ok &= expect(isKey(window_input_events::translateAppCommand(
                         appCommand(APPCOMMAND_MEDIA_PLAY_PAUSE),
                         SystemMediaCommandOwner::LocalInputFallback).event,
                     VK_MEDIA_PLAY_PAUSE),
               "the window fallback must translate a play/pause toggle");

  const auto unknown = window_input_events::translateAppCommand(
      appCommand(APPCOMMAND_VOLUME_MUTE),
      SystemMediaCommandOwner::SystemMediaTransportControls);
  ok &= expect(!unknown.handled && !unknown.event,
               "unowned application commands must remain available to the "
               "default window procedure");

  const std::vector<PlaybackAction> localActions = routeNativeMediaGesture(
      SystemMediaCommandOwner::LocalInputFallback, VK_MEDIA_PLAY_PAUSE,
      APPCOMMAND_MEDIA_PLAY_PAUSE);
  ok &= expect(localActions.size() == 1 &&
                   localActions.front() == PlaybackAction::TogglePause,
               "one native media-key gesture must yield exactly one local "
               "transport action");
  const std::vector<PlaybackAction> externallyOwnedActions =
      routeNativeMediaGesture(
          SystemMediaCommandOwner::SystemMediaTransportControls,
          VK_MEDIA_PLAY_PAUSE, APPCOMMAND_MEDIA_PLAY_PAUSE);
  ok &= expect(externallyOwnedActions.empty(),
               "native key and app-command messages must yield no local "
               "transport action while SMTC owns the command");

  const auto localConsoleAction = routeConsoleMediaKey(
      SystemMediaCommandOwner::LocalInputFallback, VK_MEDIA_PLAY_PAUSE);
  const auto externallyOwnedConsoleAction = routeConsoleMediaKey(
      SystemMediaCommandOwner::SystemMediaTransportControls,
      VK_MEDIA_PLAY_PAUSE);
  ok &= expect(localConsoleAction == PlaybackAction::TogglePause,
               "the terminal fallback must retain media-key transport");
  ok &= expect(!externallyOwnedConsoleAction,
               "terminal media keys must not duplicate SMTC commands");

  const std::optional<InputEvent> resize =
      window_input_events::textGridResizeEvent(801, 601, 10, 20);
  ok &= expect(resize && resize->type == InputEvent::Type::Resize &&
                   resize->size.X == 81 && resize->size.Y == 31,
               "native client resize must publish the new rounded-up text "
               "grid dimensions");
  ok &= expect(!window_input_events::textGridResizeEvent(0, 600, 10, 20),
               "an empty native client area must not publish a resize");
  const std::optional<InputEvent> hugeResize =
      window_input_events::textGridResizeEvent(
          (std::numeric_limits<int>::max)(),
          (std::numeric_limits<int>::max)(), 1, 1);
  ok &= expect(hugeResize &&
                   hugeResize->size.X ==
                       (std::numeric_limits<SHORT>::max)() &&
                   hugeResize->size.Y ==
                       (std::numeric_limits<SHORT>::max)(),
               "native resize dimensions must stay representable in COORD");

  return ok ? 0 : 1;
}
