#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <optional>

#include "consoleinput.h"

namespace window_input_events {

bool isKeyDownMessage(UINT message, WPARAM key);
bool isSuppressedSystemCharacter(UINT message, WPARAM key);

InputEvent keyFromVirtualKey(WORD key);

std::optional<InputEvent> inputEventFromXButton(WPARAM wParam);
std::optional<InputEvent> inputEventFromAppCommand(LPARAM lParam);

DWORD mouseButtonsFromWParam(WPARAM wParam);
DWORD wheelButtonState(SHORT delta);
InputEvent mouseEvent(int x, int y, DWORD buttonState, DWORD eventFlags);

}  // namespace window_input_events
