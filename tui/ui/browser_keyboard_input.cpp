#include "browser_keyboard_input.h"

#include <optional>
#include <utility>

#include "playback/input/match.h"

namespace browser_keyboard_input {

Result handle(BrowserState& browser, const InputEvent& event,
              const Capabilities& capabilities) {
  if (event.type != InputEvent::Type::Key) return std::monostate{};

  const KeyEvent& key = event.key;
  if (capabilities.interactionEnabled) {
    if (const std::optional<browser_input::KeyAction> action =
            browser_input::resolveKeyAction(
                key, browser_input::shortcutContext(
                         browser_input::ShortcutContext::SearchActivation))) {
      return BrowserAction{*action, keyPressCount(key)};
    }
    if (browserSearchFocused(browser)) {
      return SearchUpdate{handleBrowserSearchKey(browser, key)};
    }
  }

  if (capabilities.playbackAvailable) {
    if (std::optional<playback_input::Command> command =
            playback_input::matchShortcut(
                event, kPlaybackShortcutContextShared)) {
      return PlaybackCommand{std::move(*command)};
    }
  }

  if (const std::optional<browser_input::KeyAction> action =
          browser_input::resolveKeyAction(
              key, browser_input::shortcutContext(
                       browser_input::ShortcutContext::Application))) {
    return BrowserAction{*action, keyPressCount(key)};
  }
  if (!capabilities.interactionEnabled) return std::monostate{};

  if (const std::optional<browser_input::KeyAction> action =
          browser_input::resolveKeyAction(
              key, browser_input::shortcutContext(
                       browser_input::ShortcutContext::Navigation))) {
    return BrowserAction{*action, keyPressCount(key)};
  }
  return std::monostate{};
}

}  // namespace browser_keyboard_input
