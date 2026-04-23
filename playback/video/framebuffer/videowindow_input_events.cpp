#include "videowindow_input_events.h"

#include "playback/input/media_keys.h"

namespace videowindow_input_events {
namespace {

char characterForVirtualKey(WORD key) {
  if (key >= 'A' && key <= 'Z') {
    return static_cast<char>(key);
  }
  switch (key) {
    case VK_SPACE:
      return ' ';
    case VK_ESCAPE:
      return 27;
    case VK_OEM_4:
      return '[';
    case VK_OEM_6:
      return ']';
    default:
      return 0;
  }
}

DWORD currentModifierState() {
  DWORD control = 0;
  if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
    control |= LEFT_CTRL_PRESSED;
  }
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    control |= SHIFT_PRESSED;
  }
  if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    control |= LEFT_ALT_PRESSED;
  }
  return control;
}

InputEvent keyEvent(WORD key, char character = 0, DWORD control = 0) {
  InputEvent event{};
  event.type = InputEvent::Type::Key;
  event.key.vk = key;
  event.key.ch = character;
  event.key.control = control;
  return event;
}

}  // namespace

bool isKeyDownMessage(UINT message, WPARAM key) {
  return message == WM_KEYDOWN ||
         (message == WM_SYSKEYDOWN && key == VK_RETURN);
}

bool isSuppressedSystemCharacter(UINT message, WPARAM key) {
  return message == WM_SYSCHAR && key == VK_RETURN;
}

InputEvent keyFromVirtualKey(WORD key) {
  return keyEvent(key, characterForVirtualKey(key), currentModifierState());
}

bool isXButtonMessage(UINT message) {
  return message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
         message == WM_NCXBUTTONDOWN || message == WM_NCXBUTTONUP;
}

std::optional<InputEvent> keyFromXButtonMessage(WPARAM wParam) {
  const WORD button = HIWORD(wParam);
  if (button == XBUTTON1) {
    return keyEvent(VK_BROWSER_BACK);
  }
  if (button == XBUTTON2) {
    return keyEvent(VK_BROWSER_FORWARD);
  }
  return std::nullopt;
}

std::optional<InputEvent> keyFromAppCommand(LPARAM lParam) {
  const int command = GET_APPCOMMAND_LPARAM(lParam);
  switch (command) {
    case APPCOMMAND_BROWSER_BACKWARD:
      return keyEvent(VK_BROWSER_BACK);
    case APPCOMMAND_BROWSER_FORWARD:
      return keyEvent(VK_BROWSER_FORWARD);
    case APPCOMMAND_MEDIA_PLAY_PAUSE:
      return keyEvent(VK_MEDIA_PLAY_PAUSE);
    case APPCOMMAND_MEDIA_PLAY:
      return keyEvent(kPlaybackVkMediaPlay);
    case APPCOMMAND_MEDIA_PAUSE:
      return keyEvent(kPlaybackVkMediaPause);
    case APPCOMMAND_MEDIA_STOP:
      return keyEvent(VK_MEDIA_STOP);
    case APPCOMMAND_MEDIA_PREVIOUSTRACK:
    case APPCOMMAND_MEDIA_CHANNEL_DOWN:
      return keyEvent(VK_MEDIA_PREV_TRACK);
    case APPCOMMAND_MEDIA_NEXTTRACK:
    case APPCOMMAND_MEDIA_CHANNEL_UP:
      return keyEvent(VK_MEDIA_NEXT_TRACK);
    default:
      return std::nullopt;
  }
}

DWORD mouseButtonsFromWParam(WPARAM wParam) {
  DWORD buttonState = 0;
  if ((wParam & MK_LBUTTON) != 0) {
    buttonState |= FROM_LEFT_1ST_BUTTON_PRESSED;
  }
  if ((wParam & MK_RBUTTON) != 0) {
    buttonState |= RIGHTMOST_BUTTON_PRESSED;
  }
  return buttonState;
}

DWORD wheelButtonState(SHORT delta) {
  return static_cast<DWORD>(static_cast<uint16_t>(delta)) << 16;
}

InputEvent mouseEvent(int x, int y, DWORD buttonState, DWORD eventFlags) {
  InputEvent event{};
  event.type = InputEvent::Type::Mouse;
  event.mouse.pos.X = static_cast<SHORT>(x);
  event.mouse.pos.Y = static_cast<SHORT>(y);
  event.mouse.buttonState = buttonState;
  event.mouse.eventFlags = eventFlags;
  markWindowMouseEvent(event.mouse);
  event.mouse.hasPixelPosition = true;
  event.mouse.pixelX = x;
  event.mouse.pixelY = y;
  return event;
}

}  // namespace videowindow_input_events
