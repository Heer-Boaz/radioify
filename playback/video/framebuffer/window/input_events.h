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

bool isKeyDownMessage(UINT message, WPARAM key);
bool isSuppressedSystemCharacter(UINT message, WPARAM key);

InputEvent keyFromVirtualKey(WORD key);

std::optional<InputEvent> inputEventFromXButton(WPARAM wParam);
std::optional<InputEvent> inputEventFromAppCommand(LPARAM lParam);

MouseButtons mouseButtonsFromWParam(WPARAM wParam);
InputEvent mouseEvent(int x, int y, MouseEventKind kind, MouseButtons buttons,
                      MouseButton button, int wheelDelta = 0);
InputEvent pointerLeaveEvent();

}  // namespace window_input_events
