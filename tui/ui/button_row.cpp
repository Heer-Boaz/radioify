#include "tui/ui/button_row.h"

namespace tui_button_row {

KeyboardAction resolveKeyboardAction(const KeyEvent& key) {
  constexpr DWORD kCtrlAltMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED |
                                 LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;
  if ((key.control & kCtrlAltMask) != 0) {
    return KeyboardAction::None;
  }
  switch (key.vk) {
    case VK_RETURN:
    case VK_SPACE:
      return KeyboardAction::Activate;
    case VK_LEFT:
      return KeyboardAction::SelectPrevious;
    case VK_RIGHT:
      return KeyboardAction::SelectNext;
    case VK_TAB:
      return (key.control & SHIFT_PRESSED) != 0
                 ? KeyboardAction::FocusPrevious
                 : KeyboardAction::FocusNext;
    case VK_ESCAPE:
      return KeyboardAction::Dismiss;
    default:
      return KeyboardAction::None;
  }
}

PointerInteraction PointerState::handle(const InputEvent& event,
                                        const Layout& layout) {
  PointerInteraction result;
  if (event.type == InputEvent::Type::PointerLeave) {
    result.changed = hovered_.has_value() || armed_.has_value();
    result.captured = armed_.has_value();
    reset();
    return result;
  }
  if (event.type != InputEvent::Type::Mouse) {
    return result;
  }

  const MouseEvent& mouse = event.mouse;
  const std::optional<std::size_t> hovered =
      hitTest(layout, mouse.pos.X, mouse.pos.Y);
  if (hovered_ != hovered) {
    hovered_ = hovered;
    result.changed = true;
  }

  if (mouse.kind == MouseEventKind::Press &&
      mouse.button == MouseButton::Left &&
      isMouseButtonDown(mouse, MouseButton::Left)) {
    if (armed_ != hovered) {
      armed_ = hovered;
      result.changed = true;
    }
    result.captured = armed_.has_value();
    return result;
  }

  if (mouse.kind == MouseEventKind::Release &&
      mouse.button == MouseButton::Left) {
    const std::optional<std::size_t> armed = armed_;
    result.captured = armed.has_value();
    if (armed_ && hovered_ == armed_) {
      result.activated = armed_;
    }
    if (armed_) {
      armed_.reset();
      result.changed = true;
    }
    return result;
  }
  result.captured = armed_.has_value();
  return result;
}

void PointerState::reset() {
  hovered_.reset();
  armed_.reset();
}

}  // namespace tui_button_row
