#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "input_event.h"
#include "shortcut_match.h"

namespace tui_shell_shortcuts {

enum class Action : std::uint8_t {
  ToggleCommandPalette,
  CancelMediaTask,
};

enum class Context : std::uint8_t {
  Browser = 1u << 0,
  Global = 1u << 1,
};

inline constexpr std::uint8_t context(Context value) {
  return static_cast<std::uint8_t>(value);
}

struct Binding {
  Action action;
  WORD vk = 0;
  DWORD requiredModifierMask = 0;
  DWORD forbiddenModifierMask = 0;
  std::uint8_t contexts = 0;
  std::string_view label;
};

inline constexpr std::array<Binding, 2> kBindings = {{
    {Action::ToggleCommandPalette, VK_F1, 0,
     kShortcutCtrlMask | kShortcutAltMask | kShortcutShiftMask,
     context(Context::Browser), "F1"},
    {Action::CancelMediaTask, VK_F8, 0,
     kShortcutCtrlMask | kShortcutAltMask | kShortcutShiftMask,
     context(Context::Global), "F8"},
}};

inline std::optional<Action> resolve(const InputEvent& event,
                                     std::uint8_t contexts) {
  if (event.type != InputEvent::Type::Key) {
    return std::nullopt;
  }
  for (const Binding& binding : kBindings) {
    if ((binding.contexts & contexts) != 0 &&
        matchesShortcut(event.key, binding.vk, 0, 0,
                        binding.requiredModifierMask,
                        binding.forbiddenModifierMask)) {
      return binding.action;
    }
  }
  return std::nullopt;
}

inline constexpr std::string_view label(Action action) {
  for (const Binding& binding : kBindings) {
    if (binding.action == action) {
      return binding.label;
    }
  }
  return {};
}

}  // namespace tui_shell_shortcuts
