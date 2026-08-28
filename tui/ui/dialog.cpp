#include "dialog.h"

#include <algorithm>
#include <utility>

#include "unicode_display_width.h"

namespace tui_dialog {
namespace {

std::vector<std::string> wrapText(const std::string& text, int width) {
  std::vector<std::string> lines;
  if (width <= 0) {
    return lines;
  }
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
    if (!utf8DecodeCodepoint(text, &offset, &codepoint, &startByte,
                            &endByte)) {
      break;
    }
    if (codepoint == U'\r') {
      continue;
    }
    if (codepoint == U'\n') {
      lines.emplace_back(text.substr(lineStart, lineEnd - lineStart));
      lineStart = offset;
      lineEnd = offset;
      lineWidth = 0;
      continue;
    }
    const int glyphWidth = unicodeDisplayWidth(codepoint);
    if (glyphWidth > 0 && lineWidth > 0 &&
        lineWidth + glyphWidth > width) {
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
    if (!result.empty()) {
      result.push_back({{}, TextTone::Normal});
    }
    for (std::string line : wrapText(block.text, width)) {
      result.push_back({std::move(line), block.tone});
    }
  }
  return result;
}

}  // namespace

bool Model::open(Content content) {
  if (content.buttons.empty()) {
    content.buttons.push_back({0, "Close"});
  }
  content_ = std::move(content);
  selectedButton_ = 0;
  firstVisibleLine_ = 0;
  active_ = true;
  return true;
}

bool Model::dismiss() {
  if (!active_ && content_.title.empty() && content_.text.empty() &&
      content_.buttons.empty()) {
    return false;
  }
  content_ = {};
  selectedButton_ = 0;
  firstVisibleLine_ = 0;
  active_ = false;
  return true;
}

Layout Model::layout(const Bounds& bounds) {
  Layout result;
  if (!active_ || bounds.width < 4 || bounds.height < 6) {
    return result;
  }

  const int topInset =
      std::clamp(bounds.topInset, 0, std::max(0, bounds.height - 1));
  const int availableHeight = bounds.height - topInset;
  if (availableHeight < 6) {
    return result;
  }

  const int desiredWidth = std::clamp(bounds.width - 4, 44, 84);
  result.width = std::min(bounds.width, desiredWidth);
  result.innerWidth = std::max(1, result.width - 4);
  result.contentLines = buildLines(content_, result.innerWidth);

  const int desiredHeight =
      std::max(6, static_cast<int>(result.contentLines.size()) + 4);
  result.height = std::min(availableHeight, desiredHeight);
  result.x = std::max(0, (bounds.width - result.width) / 2);
  result.y = topInset + std::max(0, (availableHeight - result.height) / 2);
  result.titleY = result.y + 1;
  result.contentY = result.y + 2;
  result.buttonY = result.y + result.height - 2;
  result.visibleContentRows =
      std::max(0, result.buttonY - result.contentY);

  const int maximumFirstLine =
      std::max(0, static_cast<int>(result.contentLines.size()) -
                      result.visibleContentRows);
  firstVisibleLine_ = std::clamp(firstVisibleLine_, 0, maximumFirstLine);
  result.firstContentLine = firstVisibleLine_;

  const int buttonGapCount =
      std::max(0, static_cast<int>(content_.buttons.size()) - 1);
  const int availableButtonWidth =
      std::max(1, result.width - 2 - buttonGapCount);
  const int maximumButtonWidth = std::max(
      1, availableButtonWidth /
             std::max(1, static_cast<int>(content_.buttons.size())));
  int totalButtonWidth = 0;
  for (const Button& button : content_.buttons) {
    totalButtonWidth += std::min(
        maximumButtonWidth,
        std::max(4, utf8DisplayWidth(button.label) + 4));
  }
  totalButtonWidth += buttonGapCount;
  int buttonX =
      result.x + std::max(1, (result.width - totalButtonWidth) / 2);
  for (std::size_t index = 0; index < content_.buttons.size(); ++index) {
    const int buttonWidth = std::min(
        maximumButtonWidth,
        std::max(4, utf8DisplayWidth(content_.buttons[index].label) + 4));
    result.buttons.push_back({index, buttonX, buttonWidth});
    buttonX += buttonWidth + 1;
  }
  result.valid = true;
  return result;
}

Interaction Model::handle(const InputEvent& event, const Bounds& bounds) {
  Interaction result;
  if (!active_) {
    return result;
  }

  result.consumed = true;
  if (event.type == InputEvent::Type::Action &&
      event.action == InputAction::Back) {
    result.changed = dismiss();
    result.dismissed = true;
    return result;
  }

  Layout currentLayout = layout(bounds);
  if (event.type == InputEvent::Type::Key) {
    switch (event.key.vk) {
      case VK_ESCAPE:
        result.changed = dismiss();
        result.dismissed = true;
        break;
      case VK_LEFT:
        selectAdjacentButton(-1);
        result.changed = true;
        break;
      case VK_RIGHT:
      case VK_TAB:
        selectAdjacentButton(1);
        result.changed = true;
        break;
      case VK_UP:
        scrollBy(-1, currentLayout);
        result.changed = true;
        break;
      case VK_DOWN:
        scrollBy(1, currentLayout);
        result.changed = true;
        break;
      case VK_PRIOR:
        scrollBy(-std::max(1, currentLayout.visibleContentRows),
                 currentLayout);
        result.changed = true;
        break;
      case VK_NEXT:
        scrollBy(std::max(1, currentLayout.visibleContentRows),
                 currentLayout);
        result.changed = true;
        break;
      case VK_RETURN:
        return activateSelected();
      default:
        break;
    }
    return result;
  }

  if (event.type != InputEvent::Type::Mouse) {
    return result;
  }

  const MouseEvent& mouse = event.mouse;
  if (mouse.kind == MouseEventKind::VerticalWheel) {
    if (mouse.wheelDelta != 0) {
      scrollBy(mouse.wheelDelta > 0 ? -3 : 3, currentLayout);
      result.changed = true;
    }
    return result;
  }

  std::optional<std::size_t> hoveredButton;
  if (currentLayout.valid && mouse.pos.Y == currentLayout.buttonY) {
    for (const ButtonBounds& button : currentLayout.buttons) {
      if (mouse.pos.X >= button.x &&
          mouse.pos.X < button.x + button.width) {
        hoveredButton = button.index;
        break;
      }
    }
  }
  if (mouse.kind == MouseEventKind::Move) {
    if (hoveredButton && selectedButton_ != *hoveredButton) {
      selectedButton_ = *hoveredButton;
      result.changed = true;
    }
    return result;
  }
  if (mouse.kind == MouseEventKind::Press && hoveredButton &&
      isMouseButtonDown(mouse, MouseButton::Left)) {
    selectedButton_ = *hoveredButton;
    return activateSelected();
  }
  return result;
}

void Model::selectAdjacentButton(int direction) {
  if (content_.buttons.empty()) {
    selectedButton_ = 0;
    return;
  }
  const int buttonCount = static_cast<int>(content_.buttons.size());
  int selected = static_cast<int>(selectedButton_);
  selected = (selected + direction) % buttonCount;
  if (selected < 0) {
    selected += buttonCount;
  }
  selectedButton_ = static_cast<std::size_t>(selected);
}

void Model::scrollBy(int rows, const Layout& layout) {
  const int maximumFirstLine =
      std::max(0, static_cast<int>(layout.contentLines.size()) -
                      layout.visibleContentRows);
  firstVisibleLine_ =
      std::clamp(firstVisibleLine_ + rows, 0, maximumFirstLine);
}

Interaction Model::activateSelected() {
  Interaction result;
  result.consumed = true;
  if (content_.buttons.empty()) {
    return result;
  }
  selectedButton_ =
      std::min(selectedButton_, content_.buttons.size() - 1);
  result.activatedButton = content_.buttons[selectedButton_].id;
  result.changed = dismiss();
  result.dismissed = true;
  return result;
}

}  // namespace tui_dialog
