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

struct KeyEvent {
  WORD vk = 0;
  char ch = 0;
  DWORD control = 0;
};

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

struct MouseEvent {
  COORD pos{};
  DWORD buttonState = 0;
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
