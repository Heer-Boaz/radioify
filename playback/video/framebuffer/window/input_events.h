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

namespace window_input_events {

enum class SystemMediaInputPolicy {
  Ignore,
  Translate,
};

struct AppCommandTranslation {
  bool handled = false;
  std::optional<InputEvent> event;
};

bool isKeyDownMessage(UINT message, WPARAM key);
bool isSuppressedSystemCharacter(UINT message, WPARAM key);
bool isSystemMediaVirtualKey(WORD key);

InputEvent keyFromVirtualKey(WORD key);

std::optional<InputEvent> inputEventFromXButton(WPARAM wParam);
AppCommandTranslation translateAppCommand(
    LPARAM lParam, SystemMediaInputPolicy mediaPolicy);

MouseButtons mouseButtonsFromWParam(WPARAM wParam);
InputEvent mouseEvent(int x, int y, MouseEventKind kind, MouseButtons buttons,
                      MouseButton button, int wheelDelta = 0);
InputEvent pointerLeaveEvent();

}  // namespace window_input_events
