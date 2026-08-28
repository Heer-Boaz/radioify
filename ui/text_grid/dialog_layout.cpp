#include "ui/text_grid/dialog_layout.h"

#include <algorithm>
#include <utility>

#include "core/unicode_display_width.h"

namespace text_grid_dialog_layout {
namespace {

std::vector<std::string> wrapText(const std::string& text, int width) {
  std::vector<std::string> lines;
  if (width <= 0) return lines;
  if (text.empty()) {
    lines.emplace_back();
    return lines;
  }

  std::size_t offset = 0;
  std::size_t lineStart = 0;
  std::size_t lineEnd = 0;
  int lineWidth = 0;
  while (offset < text.size()) {
    char32_t codepoint = 0;
    std::size_t startByte = 0;
    std::size_t endByte = 0;
    if (!utf8DecodeCodepoint(text, &offset, &codepoint, &startByte, &endByte)) {
      break;
    }
    if (codepoint == U'\r') continue;
    if (codepoint == U'\n') {
      lines.emplace_back(text.substr(lineStart, lineEnd - lineStart));
      lineStart = offset;
      lineEnd = offset;
      lineWidth = 0;
      continue;
    }
    const int glyphWidth = unicodeDisplayWidth(codepoint);
    if (glyphWidth > 0 && lineWidth > 0 && lineWidth + glyphWidth > width) {
      lines.emplace_back(text.substr(lineStart, lineEnd - lineStart));
      lineStart = startByte;
      lineEnd = startByte;
      lineWidth = 0;
    }
    lineEnd = endByte;
    lineWidth += glyphWidth;
  }
  lines.emplace_back(text.substr(lineStart, lineEnd - lineStart));
  return lines;
}

std::vector<RenderLine> buildLines(const Content& content, int width) {
  std::vector<RenderLine> result;
  for (const TextBlock& block : content.text) {
    if (!result.empty()) result.push_back({{}, TextTone::Normal});
    for (std::string line : wrapText(block.text, width)) {
      result.push_back({std::move(line), block.tone});
    }
  }
  return result;
}

}  // namespace

Layout layoutContent(const Content& content, std::size_t selectedButton,
                     int firstVisibleLine, const Bounds& bounds) {
  Layout result;
  if (bounds.width < 4 || bounds.height < 4) return result;

  const int requestedTopInset =
      std::clamp(bounds.topInset, 0, std::max(0, bounds.height - 1));
  // A modal decision takes precedence over persistent chrome. On a short
  // surface, reclaim those rows before considering the dialog unrenderable.
  const int topInset =
      std::min(requestedTopInset, std::max(0, bounds.height - 4));
  const int availableHeight = bounds.height - topInset;
  if (availableHeight < 4) return result;

  const int desiredWidth = std::clamp(bounds.width - 4, 44, 84);
  result.width = std::min(bounds.width, desiredWidth);
  result.x = std::max(0, (bounds.width - result.width) / 2);
  result.innerWidth = std::max(1, result.width - 4);
  result.contentLines = buildLines(content, result.innerWidth);

  const std::size_t selected =
      content.buttons.empty()
          ? 0
          : std::min(selectedButton, content.buttons.size() - 1);
  auto arrangeButtons = [&](int bottomY, int maximumRows) {
    text_grid_button_layout::Layout buttons =
        text_grid_button_layout::responsiveLayout(
            content.buttons, result.x, result.width, bottomY, maximumRows);
    if (!buttons.buttons.empty() || content.buttons.empty()) return buttons;

    // At the smallest usable sizes, retain one complete selected command as
    // a viewport onto the action group. Navigation still reaches every action,
    // and resizing restores the full row or stack.
    const std::vector<Button> selectedOnly{content.buttons[selected]};
    buttons = text_grid_button_layout::responsiveLayout(
        selectedOnly, result.x, result.width, bottomY, 1);
    if (!buttons.buttons.empty()) buttons.buttons.front().index = selected;
    return buttons;
  };

  const int maximumButtonRows = std::max(1, availableHeight - 3);
  text_grid_button_layout::Layout buttonLayout =
      arrangeButtons(0, maximumButtonRows);
  if (buttonLayout.buttons.empty()) return {};

  const int buttonRows = std::max(1, buttonLayout.rowCount);
  const int minimumHeight = buttonRows + 3;
  const int desiredHeight =
      std::max(minimumHeight,
               static_cast<int>(result.contentLines.size()) + buttonRows + 3);
  result.height = std::min(availableHeight, desiredHeight);
  result.y = topInset + std::max(0, (availableHeight - result.height) / 2);
  result.titleY = result.y + 1;
  result.contentY = result.y + 2;
  result.buttonY = result.y + result.height - 2;
  buttonLayout = arrangeButtons(result.buttonY, maximumButtonRows);
  const int firstButtonY =
      buttonLayout.buttons.empty() ? result.buttonY : buttonLayout.y;
  result.visibleContentRows = std::max(0, firstButtonY - result.contentY);

  const int maximumFirstLine =
      std::max(0, static_cast<int>(result.contentLines.size()) -
                      result.visibleContentRows);
  result.firstContentLine = std::clamp(firstVisibleLine, 0, maximumFirstLine);
  result.buttons = std::move(buttonLayout.buttons);
  result.valid = !result.buttons.empty();
  return result;
}

}  // namespace text_grid_dialog_layout
