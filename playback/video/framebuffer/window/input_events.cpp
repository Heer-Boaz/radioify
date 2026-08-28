#include "input_events.h"

#include "playback/input/media_keys.h"

namespace window_input_events {
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
    case VK_OEM_COMMA:
      return ',';
    case VK_OEM_PERIOD:
      return '.';
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

bool isSystemMediaVirtualKey(WORD key) {
  switch (key) {
    case VK_MEDIA_PLAY_PAUSE:
    case VK_MEDIA_STOP:
    case VK_MEDIA_PREV_TRACK:
    case VK_MEDIA_NEXT_TRACK:
      return true;
    default:
      return false;
  }
}

InputEvent keyFromVirtualKey(WORD key) {
  if (key == VK_BROWSER_BACK) {
    return inputActionEvent(InputAction::Back);
  }
  if (key == VK_BROWSER_FORWARD) {
    return inputActionEvent(InputAction::Forward);
  }
  return keyEvent(key, characterForVirtualKey(key), currentModifierState());
}

std::optional<InputEvent> inputEventFromXButton(WPARAM wParam) {
  switch (GET_XBUTTON_WPARAM(wParam)) {
    case XBUTTON1:
      return inputActionEvent(InputAction::Back);
    case XBUTTON2:
      return inputActionEvent(InputAction::Forward);
    default:
      return std::nullopt;
  }
}

AppCommandTranslation translateAppCommand(
    LPARAM lParam, SystemMediaInputPolicy mediaPolicy) {
  const int command = GET_APPCOMMAND_LPARAM(lParam);
  switch (command) {
    case APPCOMMAND_BROWSER_BACKWARD:
      return {true, inputActionEvent(InputAction::Back)};
    case APPCOMMAND_BROWSER_FORWARD:
      return {true, inputActionEvent(InputAction::Forward)};
    case APPCOMMAND_MEDIA_PLAY_PAUSE:
      return {true, mediaPolicy == SystemMediaInputPolicy::Translate
                        ? std::optional<InputEvent>(
                              keyEvent(VK_MEDIA_PLAY_PAUSE))
                        : std::nullopt};
    case APPCOMMAND_MEDIA_PLAY:
      return {true, mediaPolicy == SystemMediaInputPolicy::Translate
                        ? std::optional<InputEvent>(
                              keyEvent(kPlaybackVkMediaPlay))
                        : std::nullopt};
    case APPCOMMAND_MEDIA_PAUSE:
      return {true, mediaPolicy == SystemMediaInputPolicy::Translate
                        ? std::optional<InputEvent>(
                              keyEvent(kPlaybackVkMediaPause))
                        : std::nullopt};
    case APPCOMMAND_MEDIA_STOP:
      return {true, mediaPolicy == SystemMediaInputPolicy::Translate
                        ? std::optional<InputEvent>(keyEvent(VK_MEDIA_STOP))
                        : std::nullopt};
    case APPCOMMAND_MEDIA_PREVIOUSTRACK:
    case APPCOMMAND_MEDIA_CHANNEL_DOWN:
      return {true, mediaPolicy == SystemMediaInputPolicy::Translate
                        ? std::optional<InputEvent>(
                              keyEvent(VK_MEDIA_PREV_TRACK))
                        : std::nullopt};
    case APPCOMMAND_MEDIA_NEXTTRACK:
    case APPCOMMAND_MEDIA_CHANNEL_UP:
      return {true, mediaPolicy == SystemMediaInputPolicy::Translate
                        ? std::optional<InputEvent>(
                              keyEvent(VK_MEDIA_NEXT_TRACK))
                        : std::nullopt};
    default:
      return {};
  }
}

MouseButtons mouseButtonsFromWParam(WPARAM wParam) {
  MouseButtons buttons = MouseButtons::None;
  if ((wParam & MK_LBUTTON) != 0) {
    buttons = buttons | MouseButtons::Left;
  }
  if ((wParam & MK_MBUTTON) != 0) {
    buttons = buttons | MouseButtons::Middle;
  }
  if ((wParam & MK_RBUTTON) != 0) {
    buttons = buttons | MouseButtons::Right;
  }
  return buttons;
}

InputEvent mouseEvent(int x, int y, MouseEventKind kind, MouseButtons buttons,
                      MouseButton button, int wheelDelta) {
  InputEvent event{};
  event.type = InputEvent::Type::Mouse;
  event.mouse.pos.X = static_cast<SHORT>(x);
  event.mouse.pos.Y = static_cast<SHORT>(y);
  event.mouse.buttons = buttons;
  event.mouse.button = button;
  event.mouse.kind = kind;
  event.mouse.wheelDelta = wheelDelta;
  markWindowMouseEvent(event.mouse);
  event.mouse.hasPixelPosition = true;
  event.mouse.pixelX = x;
  event.mouse.pixelY = y;
  return event;
}

InputEvent pointerLeaveEvent() {
  InputEvent event{};
  event.type = InputEvent::Type::PointerLeave;
  return event;
}

}  // namespace window_input_events
