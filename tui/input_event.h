#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>

#include "core/file_drop_event.h"
#include "playback/input/input_action.h"

enum class KeyPressKind : std::uint8_t {
  Initial,
  AutoRepeat,
};

struct KeyEvent {
  WORD vk = 0;
  char ch = 0;
  DWORD control = 0;
  KeyPressKind pressKind = KeyPressKind::Initial;
  // One Windows input record/message may represent multiple physical repeat
  // ticks. Keep that multiplicity attached to the gesture so consumers can
  // apply it without expanding a stale FIFO backlog.
  std::uint32_t repeatCount = 1;
};

inline constexpr bool isAutoRepeat(const KeyEvent& key) {
  return key.pressKind == KeyPressKind::AutoRepeat;
}

inline constexpr std::uint32_t keyPressCount(const KeyEvent& key) {
  return key.repeatCount == 0 ? 1 : key.repeatCount;
}

enum class MouseEventKind : uint8_t {
  Press,
  Release,
  Move,
  DoubleClick,
  VerticalWheel,
  HorizontalWheel,
  Unknown,
};

enum class MouseEventSource : uint8_t {
  Terminal,
  NativeWindow,
};

enum class MouseButton : uint8_t {
  None,
  Left,
  Middle,
  Right,
};

enum class MouseButtons : uint8_t {
  None = 0,
  Left = 1 << 0,
  Middle = 1 << 1,
  Right = 1 << 2,
};

inline constexpr MouseButtons operator|(MouseButtons lhs, MouseButtons rhs) {
  return static_cast<MouseButtons>(static_cast<uint8_t>(lhs) |
                                   static_cast<uint8_t>(rhs));
}

inline constexpr MouseButtons mouseButtonSet(MouseButton button) {
  switch (button) {
    case MouseButton::Left:
      return MouseButtons::Left;
    case MouseButton::Middle:
      return MouseButtons::Middle;
    case MouseButton::Right:
      return MouseButtons::Right;
    case MouseButton::None:
      return MouseButtons::None;
  }
  return MouseButtons::None;
}

inline constexpr bool containsMouseButton(MouseButtons buttons,
                                          MouseButton button) {
  const uint8_t mask = static_cast<uint8_t>(mouseButtonSet(button));
  return mask != 0 && (static_cast<uint8_t>(buttons) & mask) != 0;
}

struct MouseEvent {
  COORD pos{};
  // Buttons held after this event, matching the PointerEvent `buttons` model.
  MouseButtons buttons = MouseButtons::None;
  // Button whose state changed; None for motion and wheel events.
  MouseButton button = MouseButton::None;
  MouseEventKind kind = MouseEventKind::Unknown;
  MouseEventSource source = MouseEventSource::Terminal;
  int wheelDelta = 0;
  DWORD control = 0;
  bool hasPixelPosition = false;
  int pixelX = 0;
  int pixelY = 0;
  double unitWidth = 1.0;
  double unitHeight = 1.0;
};

inline constexpr bool isMouseButtonDown(const MouseEvent& mouse,
                                        MouseButton button) {
  return containsMouseButton(mouse.buttons, button);
}

inline bool isWindowMouseEvent(const MouseEvent& mouse) {
  return mouse.source == MouseEventSource::NativeWindow;
}

inline void markWindowMouseEvent(MouseEvent& mouse) {
  mouse.source = MouseEventSource::NativeWindow;
}

inline void clearWindowMouseEvent(MouseEvent& mouse) {
  mouse.source = MouseEventSource::Terminal;
}

struct InputEvent {
  enum class Type {
    None,
    Key,
    Action,
    Mouse,
    PointerLeave,
    Resize,
    FileDrop,
  };

  Type type = Type::None;
  KeyEvent key{};
  InputAction action = InputAction::Back;
  MouseEvent mouse{};
  COORD size{};
  FileDropEvent fileDrop;
};

inline InputEvent inputActionEvent(InputAction action) {
  InputEvent event{};
  event.type = InputEvent::Type::Action;
  event.action = action;
  return event;
}
