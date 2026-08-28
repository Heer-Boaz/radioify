#include "ui/text_grid/button_layout.h"

#include <algorithm>

#include "core/unicode_display_width.h"

namespace text_grid_button_layout {
namespace {

int buttonWidth(const Button& button, bool compact) {
  const std::string& label = compact && !button.compactLabel.empty()
                                 ? button.compactLabel
                                 : button.label;
  return std::max(4, utf8DisplayWidth(label) + 4);
}

Layout horizontalLayout(const std::vector<Button>& buttons, int x, int width,
                        int y, bool compact) {
  Layout result;
  result.y = y;
  if (buttons.empty() || width <= 2) return result;

  const int gapCount = std::max(0, static_cast<int>(buttons.size()) - 1);
  std::vector<int> buttonWidths;
  buttonWidths.reserve(buttons.size());
  int totalWidth = gapCount;
  for (const Button& button : buttons) {
    const int measured = buttonWidth(button, compact);
    buttonWidths.push_back(measured);
    totalWidth += measured;
  }
  if (totalWidth > width - 2) return result;

  int buttonX = x + std::max(1, (width - totalWidth) / 2);
  result.rowCount = 1;
  result.buttons.reserve(buttons.size());
  for (std::size_t index = 0; index < buttons.size(); ++index) {
    const int measured = buttonWidths[index];
    result.buttons.push_back({index, buttonX, measured, y, compact});
    buttonX += measured + 1;
  }
  return result;
}

}  // namespace

Layout layout(const std::vector<Button>& buttons, int x, int width, int y) {
  // Never publish a hitbox for an unrecognizable fragment such as "[". A
  // containing surface can choose a compact or stacked presentation instead.
  return horizontalLayout(buttons, x, width, y, false);
}

Layout responsiveLayout(const std::vector<Button>& buttons, int x, int width,
                        int bottomY, int maxRows) {
  Layout result = horizontalLayout(buttons, x, width, bottomY, false);
  if (!result.buttons.empty() || buttons.empty() || maxRows <= 0) return result;

  const bool hasCompactLabels = std::all_of(
      buttons.begin(), buttons.end(),
      [](const Button& button) { return !button.compactLabel.empty(); });
  if (hasCompactLabels) {
    result = horizontalLayout(buttons, x, width, bottomY, true);
    if (!result.buttons.empty()) return result;
  }

  if (static_cast<int>(buttons.size()) > maxRows || width <= 2) return {};

  std::vector<int> widths;
  std::vector<bool> compact;
  widths.reserve(buttons.size());
  compact.reserve(buttons.size());
  for (const Button& button : buttons) {
    int measured = buttonWidth(button, false);
    bool useCompact = false;
    if (measured > width - 2 && !button.compactLabel.empty()) {
      measured = buttonWidth(button, true);
      useCompact = true;
    }
    if (measured > width - 2) return {};
    widths.push_back(measured);
    compact.push_back(useCompact);
  }

  result.y = bottomY - static_cast<int>(buttons.size()) + 1;
  result.rowCount = static_cast<int>(buttons.size());
  result.buttons.reserve(buttons.size());
  for (std::size_t index = 0; index < buttons.size(); ++index) {
    const int measured = widths[index];
    const int rowY = result.y + static_cast<int>(index);
    const int buttonX = x + std::max(1, (width - measured) / 2);
    result.buttons.push_back({index, buttonX, measured, rowY, compact[index]});
  }
  return result;
}

const std::string& labelFor(const Button& button, const Placement& placement) {
  return placement.compact && !button.compactLabel.empty() ? button.compactLabel
                                                           : button.label;
}

std::optional<std::size_t> hitTest(const Layout& layout, int x, int y) {
  for (const Placement& button : layout.buttons) {
    const int buttonY = button.y >= 0 ? button.y : layout.y;
    if (y == buttonY && x >= button.x && x < button.x + button.width) {
      return button.index;
    }
  }
  return std::nullopt;
}

std::size_t selectAdjacent(std::size_t selected, std::size_t buttonCount,
                           int direction) {
  if (buttonCount == 0) return 0;
  const int count = static_cast<int>(buttonCount);
  int next = static_cast<int>(std::min(selected, buttonCount - 1));
  next = (next + direction) % count;
  if (next < 0) next += count;
  return static_cast<std::size_t>(next);
}

}  // namespace text_grid_button_layout
