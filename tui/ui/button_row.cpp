#include "tui/ui/button_row.h"

#include <algorithm>

#include "core/unicode_display_width.h"

namespace tui_button_row {

Layout layout(const std::vector<Button>& buttons, int x, int width, int y) {
  Layout result;
  result.y = y;
  if (buttons.empty() || width <= 2) {
    return result;
  }

  const int gapCount = std::max(0, static_cast<int>(buttons.size()) - 1);
  const int availableWidth = std::max(1, width - 2 - gapCount);
  const int maximumButtonWidth =
      std::max(1, availableWidth / static_cast<int>(buttons.size()));
  int totalWidth = gapCount;
  for (const Button& button : buttons) {
    totalWidth += std::min(
        maximumButtonWidth,
        std::max(4, utf8DisplayWidth(button.label) + 4));
  }

  int buttonX = x + std::max(1, (width - totalWidth) / 2);
  result.buttons.reserve(buttons.size());
  for (std::size_t index = 0; index < buttons.size(); ++index) {
    const int buttonWidth = std::min(
        maximumButtonWidth,
        std::max(4, utf8DisplayWidth(buttons[index].label) + 4));
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
