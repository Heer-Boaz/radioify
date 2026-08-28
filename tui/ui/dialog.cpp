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
    if (!utf8DecodeCodepoint(text, &offset, &codepoint, &startByte, &endByte)) {
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

DialogId Model::open(Content content) {
  if (content.buttons.empty()) {
    content.buttons.push_back({0, "Close"});
  }
  content_ = std::move(content);
  selectedButton_ = 0;
  if (content_.initiallySelectedButton) {
    const auto selected =
        std::find_if(content_.buttons.begin(), content_.buttons.end(),
                     [&](const Button& button) {
                       return button.id == *content_.initiallySelectedButton;
                     });
    if (selected != content_.buttons.end()) {
      selectedButton_ = static_cast<std::size_t>(
          std::distance(content_.buttons.begin(), selected));
    }
  }
  firstVisibleLine_ = 0;
  buttonPointer_.reset();
  activeDialog_ = DialogId{nextDialogId_++};
  if (nextDialogId_ == 0) {
    nextDialogId_ = 1;
  }
  active_ = true;
  return activeDialog_;
}

bool Model::dismiss() {
  if (!active_ && content_.title.empty() && content_.text.empty() &&
      content_.buttons.empty()) {
    return false;
  }
  content_ = {};
  selectedButton_ = 0;
  firstVisibleLine_ = 0;
  buttonPointer_.reset();
  active_ = false;
  activeDialog_ = {};
  return true;
}

bool Model::dismiss(DialogId expectedDialog) {
  return active_ && activeDialog_ == expectedDialog && dismiss();
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
  result.visibleContentRows = std::max(0, result.buttonY - result.contentY);

  const int maximumFirstLine =
      std::max(0, static_cast<int>(result.contentLines.size()) -
                      result.visibleContentRows);
  firstVisibleLine_ = std::clamp(firstVisibleLine_, 0, maximumFirstLine);
  result.firstContentLine = firstVisibleLine_;

  result.buttons = tui_button_row::layout(content_.buttons, result.x,
                                          result.width, result.buttonY)
                       .buttons;
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
    result.dismissedDialog = activeDialog_;
    result.changed = dismiss();
    return result;
  }

  Layout currentLayout = layout(bounds);
  if (event.type == InputEvent::Type::Key) {
    switch (event.key.vk) {
      case VK_ESCAPE:
        result.dismissedDialog = activeDialog_;
        result.changed = dismiss();
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
        scrollBy(-std::max(1, currentLayout.visibleContentRows), currentLayout);
        result.changed = true;
        break;
      case VK_NEXT:
        scrollBy(std::max(1, currentLayout.visibleContentRows), currentLayout);
        result.changed = true;
        break;
      case VK_RETURN:
        return activateSelected();
      default:
        break;
    }
    return result;
  }

  if (event.type != InputEvent::Type::Mouse &&
      event.type != InputEvent::Type::PointerLeave) {
    return result;
  }

  const tui_button_row::PointerInteraction pointer = buttonPointer_.handle(
      event, {currentLayout.buttonY, currentLayout.buttons});
  result.changed = pointer.changed;
  if (buttonPointer_.hovered() &&
      selectedButton_ != *buttonPointer_.hovered()) {
    selectedButton_ = *buttonPointer_.hovered();
    result.changed = true;
  }
  if (pointer.activated) {
    selectedButton_ = *pointer.activated;
    return activateSelected();
  }
  if (event.type == InputEvent::Type::PointerLeave) {
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

  return result;
}

void Model::selectAdjacentButton(int direction) {
  selectedButton_ = tui_button_row::selectAdjacent(
      selectedButton_, content_.buttons.size(), direction);
}

void Model::scrollBy(int rows, const Layout& layout) {
  const int maximumFirstLine =
      std::max(0, static_cast<int>(layout.contentLines.size()) -
                      layout.visibleContentRows);
  firstVisibleLine_ = std::clamp(firstVisibleLine_ + rows, 0, maximumFirstLine);
}

Interaction Model::activateSelected() {
  Interaction result;
  result.consumed = true;
  if (content_.buttons.empty()) {
    return result;
  }
  selectedButton_ = std::min(selectedButton_, content_.buttons.size() - 1);
  result.activation =
      ButtonActivation{activeDialog_, content_.buttons[selectedButton_].id};
  result.dismissedDialog = activeDialog_;
  result.changed = dismiss();
  return result;
}

}  // namespace tui_dialog
