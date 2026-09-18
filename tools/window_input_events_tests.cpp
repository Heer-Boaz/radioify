#include <chrono>
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

SystemMediaCommandArbiter::Clock::time_point mediaPressAt(int milliseconds) {
  return SystemMediaCommandArbiter::Clock::time_point{} +
         std::chrono::milliseconds(milliseconds);
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

// One physical media-key press as the focused window observes it: a key-down
// that DefWindowProc turns into WM_APPCOMMAND, followed by the auto-repeat that
// must not become a second press.
std::vector<PlaybackAction> routeFocusedSurfaceMediaGesture(
    WORD virtualKey, int appCommandId) {
  std::vector<PlaybackAction> actions;
  const auto dispatchKeyDown = [&](bool repeated) {
    const window_input_events::KeyDownTranslation translation =
        window_input_events::translateKeyDown(virtualKey, keyDown(repeated));
    appendPlaybackAction(translation.event, actions);
    if (translation.route !=
        window_input_events::KeyDownRoute::DelegateToDefaultWindowProcedure) {
      return;
    }
    const window_input_events::AppCommandTranslation appCommandTranslation =
        window_input_events::translateAppCommand(appCommand(appCommandId));
    if (!appCommandTranslation.event) return;
    if (!admitLocalMediaVirtualKey(appCommandTranslation.event->key.vk)) return;
    appendPlaybackAction(appCommandTranslation.event, actions);
  };
  dispatchKeyDown(false);
  dispatchKeyDown(true);
  return actions;
}

std::vector<PlaybackAction> routeNativeKeyGesture(
    WORD virtualKey,
    std::uint32_t contexts = kPlaybackShortcutContextGlobal |
                             kPlaybackShortcutContextShared) {
  std::vector<PlaybackAction> actions;
  for (const bool repeated : {false, true}) {
    const window_input_events::KeyDownTranslation translation =
        window_input_events::translateKeyDown(virtualKey, keyDown(repeated));
    appendPlaybackAction(translation.event, actions, contexts);
  }
  return actions;
}

std::optional<PlaybackAction> routeConsoleMediaKey(WORD virtualKey) {
  if (!admitLocalMediaVirtualKey(virtualKey)) {
    return std::nullopt;
  }
  return resolvePlaybackAction(
      window_input_events::keyFromVirtualKey(virtualKey).key);
}

}  // namespace

int main() {
  static_assert(
      std::is_constructible_v<VideoWindow, GpuRuntime&>,
      "a native input surface must not need a media-command owner to exist: "
      "which transport reports a press is decided per press, not at startup");
  static_assert(
      std::is_constructible_v<AudioPictureInPictureWindow, GpuRuntime&>,
      "audio picture-in-picture must not need a media-command owner to exist");

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

  const auto countedNativeSeek =
      window_input_events::translateKeyDown(VK_LEFT, keyDown(true, 6));
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

  // Media keys are grouped by the command they describe rather than the exact
  // command a transport names: Windows resolves the single play/pause key to
  // Play, Pause or PlayPause depending on the state it believes the session is
  // in, so the two ingresses must still recognise one another's report.
  ok &= expect(systemMediaCommandGroup(PlaybackControlCommand::Play) ==
                       SystemMediaCommandGroup::PlayPause &&
                   systemMediaCommandGroup(PlaybackControlCommand::Pause) ==
                       SystemMediaCommandGroup::PlayPause &&
                   systemMediaCommandGroup(
                       PlaybackControlCommand::TogglePause) ==
                       SystemMediaCommandGroup::PlayPause,
               "every play/pause spelling must share one command group");
  ok &= expect(systemMediaCommandGroup(PlaybackControlCommand::Previous) ==
                       SystemMediaCommandGroup::Previous &&
                   systemMediaCommandGroup(PlaybackControlCommand::Next) ==
                       SystemMediaCommandGroup::Next &&
                   systemMediaCommandGroup(PlaybackControlCommand::Stop) ==
                       SystemMediaCommandGroup::Stop,
               "transport commands must keep distinct command groups");
  SystemMediaCommandGroup mappedGroup = SystemMediaCommandGroup::PlayPause;
  ok &= expect(systemMediaCommandGroupForVirtualKey(VK_MEDIA_NEXT_TRACK,
                                                    &mappedGroup) &&
                   mappedGroup == SystemMediaCommandGroup::Next,
               "the next-track key must map to the next-track command group");
  ok &= expect(systemMediaCommandGroupForVirtualKey(kPlaybackVkMediaPause,
                                                    &mappedGroup) &&
                   mappedGroup == SystemMediaCommandGroup::PlayPause,
               "the synthetic pause key must map to the play/pause group");
  ok &= expect(!systemMediaCommandGroupForVirtualKey('W', &mappedGroup),
               "an ordinary key must not carry a media command group");

  {
    // The shell decides per press which media session is current, so a press
    // can reach the focused surface and never reach the SMTC session at all.
    // Refusing it because a session exists is what made media keys work only
    // some of the time.
    SystemMediaCommandArbiter arbiter;
    ok &= expect(
        arbiter.accept(SystemMediaCommandGroup::Next,
                       SystemMediaCommandIngress::FocusedSurface,
                       mediaPressAt(0)) &&
            arbiter.accept(SystemMediaCommandGroup::Next,
                           SystemMediaCommandIngress::FocusedSurface,
                           mediaPressAt(500)),
        "the focused surface must act on every media key the SMTC session "
        "never reports");
  }
  {
    SystemMediaCommandArbiter arbiter;
    ok &= expect(arbiter.accept(SystemMediaCommandGroup::Next,
                                SystemMediaCommandIngress::FocusedSurface,
                                mediaPressAt(0)),
                 "the first transport to report a press must own it");
    ok &= expect(!arbiter.accept(SystemMediaCommandGroup::Next,
                                 SystemMediaCommandIngress::SystemSession,
                                 mediaPressAt(10)),
                 "the SMTC echo of a press the focused surface handled must be "
                 "dropped");
    ok &= expect(arbiter.accept(SystemMediaCommandGroup::Next,
                                SystemMediaCommandIngress::SystemSession,
                                mediaPressAt(20)),
                 "a spent admission must not suppress the next press");
  }
  {
    SystemMediaCommandArbiter arbiter;
    ok &= expect(arbiter.accept(SystemMediaCommandGroup::PlayPause,
                                SystemMediaCommandIngress::SystemSession,
                                mediaPressAt(0)),
                 "an SMTC button press must be acted on when no focused "
                 "surface reported it");
    ok &= expect(!arbiter.accept(SystemMediaCommandGroup::PlayPause,
                                 SystemMediaCommandIngress::FocusedSurface,
                                 mediaPressAt(10)),
                 "arbitration must hold in both arrival orders");
  }
  {
    SystemMediaCommandArbiter arbiter;
    ok &= expect(arbiter.accept(SystemMediaCommandGroup::Next,
                                SystemMediaCommandIngress::SystemSession,
                                mediaPressAt(0)) &&
                     arbiter.accept(SystemMediaCommandGroup::Next,
                                    SystemMediaCommandIngress::SystemSession,
                                    mediaPressAt(120)),
                 "repeated presses on one transport are repeated presses: two "
                 "quick next-track taps must skip two tracks");
  }
  {
    SystemMediaCommandArbiter arbiter;
    ok &= expect(arbiter.accept(SystemMediaCommandGroup::Stop,
                                SystemMediaCommandIngress::FocusedSurface,
                                mediaPressAt(0)) &&
                     arbiter.accept(SystemMediaCommandGroup::Stop,
                                    SystemMediaCommandIngress::SystemSession,
                                    mediaPressAt(1000)),
                 "a command arriving long after an unrelated press must not be "
                 "mistaken for its echo");
  }
  {
    SystemMediaCommandArbiter arbiter;
    ok &= expect(arbiter.accept(SystemMediaCommandGroup::Next,
                                SystemMediaCommandIngress::FocusedSurface,
                                mediaPressAt(0)) &&
                     arbiter.accept(SystemMediaCommandGroup::PlayPause,
                                    SystemMediaCommandIngress::SystemSession,
                                    mediaPressAt(10)),
                 "one command group must not suppress another");
  }

  ok &= expect(admitLocalMediaVirtualKey('W'),
               "ordinary keys must reach the focused surface without media "
               "arbitration");

  systemMediaCommandArbiter().reset();
  const std::vector<PlaybackAction> focusedNextTrack =
      routeFocusedSurfaceMediaGesture(VK_MEDIA_NEXT_TRACK,
                                      APPCOMMAND_MEDIA_NEXTTRACK);
  ok &= expect(focusedNextTrack.size() == 1 &&
                   focusedNextTrack.front() == PlaybackAction::Next,
               "one next-track gesture on the focused window must yield "
               "exactly one transport action");
  ok &= expect(!admitSystemSessionMediaCommand(PlaybackControlCommand::Next),
               "the SMTC session must not repeat a next-track press the "
               "focused window already handled");

  systemMediaCommandArbiter().reset();
  const std::vector<PlaybackAction> focusedToggle =
      routeFocusedSurfaceMediaGesture(VK_MEDIA_PLAY_PAUSE,
                                      APPCOMMAND_MEDIA_PLAY_PAUSE);
  ok &= expect(focusedToggle.size() == 1 &&
                   focusedToggle.front() == PlaybackAction::TogglePause,
               "one native media-key gesture must yield exactly one local "
               "transport action");

  systemMediaCommandArbiter().reset();
  ok &= expect(routeConsoleMediaKey(VK_MEDIA_PREV_TRACK) ==
                   PlaybackAction::Previous,
               "the terminal surface must retain media-key transport");
  ok &= expect(
      !admitSystemSessionMediaCommand(PlaybackControlCommand::Previous),
      "the SMTC session must not repeat a press the console already handled");
  systemMediaCommandArbiter().reset();

  ok &= expect(isKey(window_input_events::translateAppCommand(
                         appCommand(APPCOMMAND_MEDIA_PAUSE)).event,
                     kPlaybackVkMediaPause),
               "the window must translate an explicit pause command");
  ok &= expect(isKey(window_input_events::translateAppCommand(
                         appCommand(APPCOMMAND_MEDIA_PLAY_PAUSE)).event,
                     VK_MEDIA_PLAY_PAUSE),
               "the window must translate a play/pause toggle");
  ok &= expect(isKey(window_input_events::translateAppCommand(
                         appCommand(APPCOMMAND_MEDIA_STOP)).event,
                     VK_MEDIA_STOP),
               "the window must translate a stop command");
  ok &= expect(isKey(window_input_events::translateAppCommand(
                         appCommand(APPCOMMAND_MEDIA_PREVIOUSTRACK)).event,
                     VK_MEDIA_PREV_TRACK),
               "the window must translate a previous-track command");
  ok &= expect(isKey(window_input_events::translateAppCommand(
                         appCommand(APPCOMMAND_MEDIA_NEXTTRACK)).event,
                     VK_MEDIA_NEXT_TRACK),
               "the window must translate a next-track command");

  const auto browserBack = window_input_events::translateAppCommand(
      appCommand(APPCOMMAND_BROWSER_BACKWARD));
  ok &= expect(browserBack.handled && browserBack.event &&
                   browserBack.event->type == InputEvent::Type::Action &&
                   browserBack.event->action == InputAction::Back,
               "browser commands must remain available alongside media "
               "commands");
  const auto initialBrowserBack =
      window_input_events::translateKeyDown(VK_BROWSER_BACK, keyDown(false));
  const auto repeatedBrowserBack =
      window_input_events::translateKeyDown(VK_BROWSER_BACK, keyDown(true));
  ok &= expect(initialBrowserBack.event &&
                   initialBrowserBack.event->type == InputEvent::Type::Action &&
                   initialBrowserBack.event->action == InputAction::Back &&
                   !repeatedBrowserBack.event,
               "holding a browser back key must not enqueue navigation "
               "backlog");

  const auto unknown = window_input_events::translateAppCommand(
      appCommand(APPCOMMAND_VOLUME_MUTE));
  ok &= expect(!unknown.handled && !unknown.event,
               "unowned application commands must remain available to the "
               "default window procedure");

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
