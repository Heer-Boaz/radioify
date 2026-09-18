#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <optional>

#include "input_event.h"
#include "playback/input/media_keys.h"

namespace window_input_events {

enum class KeyDownRoute : std::uint8_t {
  Queue,
  DelegateToDefaultWindowProcedure,
  Consume,
};

struct AppCommandTranslation {
  bool handled = false;
  std::optional<InputEvent> event;
};

struct KeyDownTranslation {
  KeyDownRoute route = KeyDownRoute::Consume;
  std::optional<InputEvent> event;
};

bool isKeyDownMessage(UINT message, WPARAM key);
bool isSuppressedSystemCharacter(UINT message, WPARAM key);
bool isRepeatedKeyDown(LPARAM lParam);
std::uint32_t keyDownRepeatCount(LPARAM lParam);
KeyDownTranslation translateKeyDown(WORD key, LPARAM lParam);

InputEvent keyFromVirtualKey(
    WORD key, KeyPressKind pressKind = KeyPressKind::Initial,
    std::uint32_t repeatCount = 1);

std::optional<InputEvent> inputEventFromXButton(WPARAM wParam);
AppCommandTranslation translateAppCommand(LPARAM lParam);

MouseButtons mouseButtonsFromWParam(WPARAM wParam);
InputEvent mouseEvent(int x, int y, MouseEventKind kind, MouseButtons buttons,
                      MouseButton button, int wheelDelta = 0);
InputEvent pointerLeaveEvent();
std::optional<InputEvent> textGridResizeEvent(int pixelWidth, int pixelHeight,
                                              int cellWidth, int cellHeight);

}  // namespace window_input_events
