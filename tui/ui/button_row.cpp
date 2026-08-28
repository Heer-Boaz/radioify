#include "tui/ui/button_row.h"

#include <algorithm>

#include "core/unicode_display_width.h"

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

Layout layout(const std::vector<Button>& buttons, int x, int width, int y) {
  Layout result;
  result.y = y;
  if (buttons.empty() || width <= 2) {
    return result;
  }

  const int gapCount = std::max(0, static_cast<int>(buttons.size()) - 1);
  std::vector<int> buttonWidths;
  buttonWidths.reserve(buttons.size());
  int totalWidth = gapCount;
  for (const Button& button : buttons) {
    const int buttonWidth = std::max(4, utf8DisplayWidth(button.label) + 4);
    buttonWidths.push_back(buttonWidth);
    totalWidth += buttonWidth;
  }
  // Never publish a hitbox for an unrecognizable fragment such as "[". A
  // containing surface can choose a compact or stacked presentation instead.
  if (totalWidth > width - 2) {
    return result;
  }

  int buttonX = x + std::max(1, (width - totalWidth) / 2);
  result.buttons.reserve(buttons.size());
  for (std::size_t index = 0; index < buttons.size(); ++index) {
    const int buttonWidth = buttonWidths[index];
    result.buttons.push_back({index, buttonX, buttonWidth});
    buttonX += buttonWidth + 1;
  }
  return result;
}

std::optional<std::size_t> hitTest(const Layout& layout, int x, int y) {
  if (y != layout.y) {
    return std::nullopt;
  }
  for (const Placement& button : layout.buttons) {
    if (x >= button.x && x < button.x + button.width) {
      return button.index;
    }
  }
  return std::nullopt;
}

std::size_t selectAdjacent(std::size_t selected, std::size_t buttonCount,
                           int direction) {
  if (buttonCount == 0) {
    return 0;
  }
  const int count = static_cast<int>(buttonCount);
  int next = static_cast<int>(std::min(selected, buttonCount - 1));
  next = (next + direction) % count;
  if (next < 0) {
    next += count;
  }
  return static_cast<std::size_t>(next);
}

}  // namespace tui_button_row
