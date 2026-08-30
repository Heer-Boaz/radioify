#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "input_event.h"
#include "shortcut_match.h"

namespace browser_input {

enum class KeyAction : std::uint8_t {
  BeginPathSearch,
  BeginFilter,
  TogglePitchMonitor,
  CycleSort,
  ToggleSortDirection,
  Stop,
  NavigateUp,
  ActivateSelection,
  OpenSelectionMenu,
  CycleView,
  MoveLeft,
  MoveRight,
  MoveUp,
  MoveDown,
  PageUp,
  PageDown,
};

enum class ShortcutContext : std::uint8_t {
  SearchActivation = 1u << 0,
  Application = 1u << 1,
  Navigation = 1u << 2,
};

inline constexpr std::uint8_t shortcutContext(ShortcutContext context) {
  return static_cast<std::uint8_t>(context);
}

struct KeyBinding {
  constexpr KeyBinding(KeyAction actionValue, WORD virtualKey,
                       char lowerCharacter, char upperCharacter,
                       DWORD requiredModifiers, DWORD forbiddenModifiers,
                       std::uint8_t shortcutContexts,
                       std::string_view label = {},
                       ShortcutRepeatPolicy repeat =
                           ShortcutRepeatPolicy::InitialPressOnly)
      : action(actionValue),
        vk(virtualKey),
        lower(lowerCharacter),
        upper(upperCharacter),
        requiredModifierMask(requiredModifiers),
        forbiddenModifierMask(forbiddenModifiers),
        contexts(shortcutContexts),
        displayLabel(label),
        repeatPolicy(repeat) {}

  KeyAction action;
  WORD vk = 0;
  char lower = 0;
  char upper = 0;
  DWORD requiredModifierMask = 0;
  DWORD forbiddenModifierMask = 0;
  std::uint8_t contexts = 0;
  std::string_view displayLabel;
  ShortcutRepeatPolicy repeatPolicy =
      ShortcutRepeatPolicy::InitialPressOnly;
};

inline constexpr std::array<KeyBinding, 17> kKeyBindings = {{
    {KeyAction::BeginPathSearch, 'G', 'g', 'G', kShortcutCtrlMask,
     kShortcutChordForbiddenMask,
     shortcutContext(ShortcutContext::SearchActivation)},
    {KeyAction::BeginFilter, 'F', 'f', 'F', kShortcutCtrlMask,
     kShortcutChordForbiddenMask,
     shortcutContext(ShortcutContext::SearchActivation)},
    {KeyAction::BeginFilter, VK_DIVIDE, '/', '/', 0,
     kShortcutTextForbiddenMask,
     shortcutContext(ShortcutContext::SearchActivation)},
    {KeyAction::TogglePitchMonitor, 'M', 'm', 'M', 0,
     kShortcutTextForbiddenMask,
     shortcutContext(ShortcutContext::Application), "M"},
    {KeyAction::ToggleSortDirection, 'S', 's', 'S', kShortcutAltMask,
     kShortcutCtrlMask | kShortcutShiftMask,
     shortcutContext(ShortcutContext::Navigation)},
    {KeyAction::CycleSort, 'S', 's', 'S', 0, kShortcutTextForbiddenMask,
     shortcutContext(ShortcutContext::Navigation)},
    {KeyAction::Stop, VK_ESCAPE, 0, 0, 0, 0,
     shortcutContext(ShortcutContext::Navigation)},
    {KeyAction::NavigateUp, VK_BACK, 0, 0, 0, 0,
     shortcutContext(ShortcutContext::Navigation)},
    {KeyAction::OpenSelectionMenu, VK_RETURN, 0, 0, kShortcutCtrlMask,
     0,
     shortcutContext(ShortcutContext::Navigation)},
    {KeyAction::ActivateSelection, VK_RETURN, 0, 0, 0, 0,
     shortcutContext(ShortcutContext::Navigation)},
    {KeyAction::CycleView, 'T', 't', 'T', 0, kShortcutTextForbiddenMask,
     shortcutContext(ShortcutContext::Navigation), "T"},
    {KeyAction::MoveLeft, VK_LEFT, 0, 0, 0, 0,
     shortcutContext(ShortcutContext::Navigation), {},
     ShortcutRepeatPolicy::AllowAutoRepeat},
    {KeyAction::MoveRight, VK_RIGHT, 0, 0, 0, 0,
     shortcutContext(ShortcutContext::Navigation), {},
     ShortcutRepeatPolicy::AllowAutoRepeat},
    {KeyAction::MoveUp, VK_UP, 0, 0, 0, 0,
     shortcutContext(ShortcutContext::Navigation), {},
     ShortcutRepeatPolicy::AllowAutoRepeat},
    {KeyAction::MoveDown, VK_DOWN, 0, 0, 0, 0,
     shortcutContext(ShortcutContext::Navigation), {},
     ShortcutRepeatPolicy::AllowAutoRepeat},
    {KeyAction::PageUp, VK_PRIOR, 0, 0, 0, 0,
     shortcutContext(ShortcutContext::Navigation), {},
     ShortcutRepeatPolicy::AllowAutoRepeat},
    {KeyAction::PageDown, VK_NEXT, 0, 0, 0, 0,
     shortcutContext(ShortcutContext::Navigation), {},
     ShortcutRepeatPolicy::AllowAutoRepeat},
}};

inline std::optional<KeyAction> resolveKeyAction(const KeyEvent& key,
                                                 std::uint8_t contexts) {
  for (const KeyBinding& binding : kKeyBindings) {
    if ((binding.contexts & contexts) == 0) {
      continue;
    }
    if (matchesShortcut(key, binding.vk, binding.lower, binding.upper,
                        binding.requiredModifierMask,
                        binding.forbiddenModifierMask,
                        binding.repeatPolicy)) {
      return binding.action;
    }
  }
  return std::nullopt;
}

inline constexpr std::string_view keyActionDisplayLabel(KeyAction action) {
  for (const KeyBinding& binding : kKeyBindings) {
    if (binding.action == action && !binding.displayLabel.empty()) {
      return binding.displayLabel;
    }
  }
  return {};
}

}  // namespace browser_input
