#include "dialog.h"

#include <algorithm>
#include <utility>

namespace tui_dialog {

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
  if (!active_) return {};
  Layout result =
      layoutContent(content_, selectedButton_, firstVisibleLine_, bounds);
  if (result.valid) firstVisibleLine_ = result.firstContentLine;
  return result;
}

Interaction Model::handle(const InputEvent& event, const Bounds& bounds) {
  Interaction result;
  if (!active_) {
    return result;
  }

  if (event.type == InputEvent::Type::Resize) {
    // Button geometry is no longer the geometry under which a press was
    // armed. Cancel it before laying out the dialog at its new size.
    buttonPointer_.reset();
    result.changed = true;
  }

  if (event.type == InputEvent::Type::Action &&
      event.action == InputAction::Back) {
    result.consumed = true;
    result.dismissedDialog = activeDialog_;
    result.changed = dismiss();
    return result;
  }

  Layout currentLayout = layout(bounds);
  if (!currentLayout.valid) {
    // Resizing is not a user decision. Preserve the modal so it reappears when
    // the surface becomes large enough, while still consuming input to prevent
    // click-through. Back/Escape was handled above and remains an explicit
    // escape hatch even while the dialog cannot be rendered.
    result.consumed = true;
    return result;
  }

  result.consumed = true;
  if (event.type == InputEvent::Type::Key) {
    const tui_button_row::KeyboardAction keyboardAction =
        tui_button_row::resolveKeyboardAction(event.key);
    switch (keyboardAction) {
      case tui_button_row::KeyboardAction::Dismiss:
        result.dismissedDialog = activeDialog_;
        result.changed = dismiss();
        break;
      case tui_button_row::KeyboardAction::SelectPrevious:
      case tui_button_row::KeyboardAction::FocusPrevious:
        selectAdjacentButton(-1);
        result.changed = true;
        break;
      case tui_button_row::KeyboardAction::SelectNext:
      case tui_button_row::KeyboardAction::FocusNext:
        selectAdjacentButton(1);
        result.changed = true;
        break;
      case tui_button_row::KeyboardAction::Activate:
        if (!currentLayout.buttons.empty()) {
          return activateSelected();
        }
        break;
      case tui_button_row::KeyboardAction::None:
        switch (event.key.vk) {
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
          default:
            break;
        }
        break;
    }
    return result;
  }

  if (event.type != InputEvent::Type::Mouse &&
      event.type != InputEvent::Type::PointerLeave) {
    return result;
  }

  const tui_button_row::PointerInteraction pointer = buttonPointer_.handle(
      event, {currentLayout.buttonY, 0, currentLayout.buttons});
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
