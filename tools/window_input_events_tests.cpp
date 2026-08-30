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
#include "tui/console_key_input.h"
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

constexpr LPARAM keyDown(bool repeated, std::uint16_t repeatCount = 1) {
  return static_cast<LPARAM>(repeatCount) |
         (repeated ? static_cast<LPARAM>(std::uint32_t{1} << 30) : 0);
}

KEY_EVENT_RECORD consoleKey(bool down, WORD virtualKey,
                            WORD repeatCount = 1, wchar_t character = 0) {
  KEY_EVENT_RECORD record{};
  record.bKeyDown = down ? TRUE : FALSE;
  record.wRepeatCount = repeatCount;
  record.wVirtualKeyCode = virtualKey;
  record.uChar.UnicodeChar = character;
  return record;
}

bool isKey(const std::optional<InputEvent>& event, WORD key) {
  return event && event->type == InputEvent::Type::Key &&
         event->key.vk == key;
}

void appendPlaybackAction(const std::optional<InputEvent>& event,
                          std::vector<PlaybackAction>& actions,
                          std::uint32_t contexts =
                              kPlaybackShortcutContextGlobal |
                              kPlaybackShortcutContextShared) {
  if (!event || event->type != InputEvent::Type::Key) return;
  if (const auto action = resolvePlaybackAction(event->key, contexts)) {
    actions.push_back(*action);
  }
}

std::vector<PlaybackAction> routeNativeMediaGesture(
    SystemMediaCommandOwner owner, WORD virtualKey, int appCommandId) {
  std::vector<PlaybackAction> actions;
  const auto dispatchKeyDown = [&](bool repeated) {
    const window_input_events::KeyDownTranslation translation =
        window_input_events::translateKeyDown(
            virtualKey, keyDown(repeated), owner);
    appendPlaybackAction(translation.event, actions);
    if (translation.route ==
        window_input_events::KeyDownRoute::DelegateToDefaultWindowProcedure) {
      // DefWindowProc emits WM_APPCOMMAND. Repeat key-downs must not reach
      // this path because WM_APPCOMMAND carries no repeat phase.
      appendPlaybackAction(
          window_input_events::translateAppCommand(
              appCommand(appCommandId), owner)
              .event,
          actions);
    }
  };
  dispatchKeyDown(false);
  dispatchKeyDown(true);
  if (owner == SystemMediaCommandOwner::SystemMediaTransportControls) {
    // A device can publish WM_APPCOMMAND directly, without a key-down routed
    // through this window. External ownership must consume that form too.
    appendPlaybackAction(
        window_input_events::translateAppCommand(
            appCommand(appCommandId), owner)
            .event,
        actions);
  }
  return actions;
}

std::vector<PlaybackAction> routeNativeKeyGesture(
    WORD virtualKey,
    std::uint32_t contexts = kPlaybackShortcutContextGlobal |
                             kPlaybackShortcutContextShared) {
  std::vector<PlaybackAction> actions;
  for (const bool repeated : {false, true}) {
    const window_input_events::KeyDownTranslation translation =
        window_input_events::translateKeyDown(
            virtualKey, keyDown(repeated),
            SystemMediaCommandOwner::LocalInputFallback);
    appendPlaybackAction(translation.event, actions, contexts);
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
  ok &= expect(!window_input_events::isRepeatedKeyDown(keyDown(false)) &&
                   window_input_events::isRepeatedKeyDown(keyDown(true)) &&
                   window_input_events::keyDownRepeatCount(
                       keyDown(true, 7)) == 7,
               "native key-down phase and batched repeat count must survive "
               "Win32 lParam translation");
  tui_console_key_input::State consoleKeyState;
  const KeyEvent firstConsoleSpace =
      consoleKeyState.keyDown(consoleKey(true, VK_SPACE, 4, L' '));
  const KeyEvent repeatedConsoleSpace =
      consoleKeyState.keyDown(consoleKey(true, VK_SPACE, 3, L' '));
  ok &= expect(firstConsoleSpace.pressKind == KeyPressKind::Initial &&
                   firstConsoleSpace.repeatCount == 4 &&
                   repeatedConsoleSpace.pressKind ==
                       KeyPressKind::AutoRepeat &&
                   repeatedConsoleSpace.repeatCount == 3,
               "a real console record must preserve lifecycle and Windows "
               "repeat multiplicity");
  consoleKeyState.keyUp(VK_SPACE);
  ok &= expect(consoleKeyState
                       .keyDown(consoleKey(true, VK_SPACE, 1, L' '))
                       .pressKind == KeyPressKind::Initial,
               "console key-up must start the next press lifecycle");
  consoleKeyState.keyDown(consoleKey(true, 'W', 1, L'w'));
  consoleKeyState.reset();
  ok &= expect(consoleKeyState.keyDown(consoleKey(true, 'W', 1, L'w'))
                       .pressKind == KeyPressKind::Initial,
               "console focus loss must reset every held-key lifecycle");

  const auto countedNativeSeek = window_input_events::translateKeyDown(
      VK_LEFT, keyDown(true, 6),
      SystemMediaCommandOwner::LocalInputFallback);
  const std::optional<PlaybackActionMatch> countedNativeSeekMatch =
      countedNativeSeek.event
          ? resolvePlaybackActionMatch(*countedNativeSeek.event)
          : std::nullopt;
  const std::optional<playback_input::Command> countedNativeSeekCommand =
      countedNativeSeekMatch
          ? std::optional<playback_input::Command>(
                playback_input::commandForShortcut(*countedNativeSeekMatch))
          : std::nullopt;
  const auto* countedNativeSeekSteps =
      countedNativeSeekCommand
          ? std::get_if<playback_input::SeekBySteps>(
                &*countedNativeSeekCommand)
          : nullptr;
  ok &= expect(countedNativeSeek.event &&
                   countedNativeSeek.event->key.repeatCount == 6 &&
                   countedNativeSeekSteps &&
                   countedNativeSeekSteps->steps == -6,
               "batched native seek repeats must reach the semantic command "
               "without FIFO expansion");

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
  const auto initialBrowserBack = window_input_events::translateKeyDown(
      VK_BROWSER_BACK, keyDown(false),
      SystemMediaCommandOwner::SystemMediaTransportControls);
  const auto repeatedBrowserBack = window_input_events::translateKeyDown(
      VK_BROWSER_BACK, keyDown(true),
      SystemMediaCommandOwner::SystemMediaTransportControls);
  ok &= expect(initialBrowserBack.event &&
                   initialBrowserBack.event->type == InputEvent::Type::Action &&
                   initialBrowserBack.event->action == InputAction::Back &&
                   !repeatedBrowserBack.event,
               "holding a browser back key must not enqueue navigation "
               "backlog");

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

  const std::vector<PlaybackAction> pauseActions =
      routeNativeKeyGesture(VK_SPACE);
  ok &= expect(pauseActions.size() == 1 &&
                   pauseActions.front() == PlaybackAction::TogglePause,
               "holding the pause toggle must dispatch only its initial "
               "press");
  const std::vector<PlaybackAction> frameStepActions =
      routeNativeKeyGesture(VK_OEM_COMMA,
                            kPlaybackShortcutContextGlobal |
                                kPlaybackShortcutContextShared |
                                kPlaybackShortcutContextPlaybackSession |
                                kPlaybackShortcutContextVideoPlayback);
  ok &= expect(frameStepActions.size() == 1 &&
                   frameStepActions.front() == PlaybackAction::PreviousFrame,
               "holding frame-step must not enqueue additional steps");
  const std::vector<PlaybackAction> seekActions =
      routeNativeKeyGesture(VK_OEM_4);
  ok &= expect(seekActions.size() == 2 &&
                   seekActions.front() == PlaybackAction::SeekBackward &&
                   seekActions.back() == PlaybackAction::SeekBackward,
               "timeline seek must retain deliberate key-repeat behavior");

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
