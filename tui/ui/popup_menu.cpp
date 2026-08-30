#include "popup_menu.h"

#include <algorithm>
#include <utility>

#include "unicode_display_width.h"

namespace tui_popup_menu {

void Model::open(std::vector<std::string> labels, Anchor anchor) {
  labels_ = std::move(labels);
  anchor_ = anchor;
  selected_ = 0;
  firstVisible_ = 0;
  active_ = !labels_.empty();
}

bool Model::dismiss() {
  if (!active_ && labels_.empty()) {
    return false;
  }
  labels_.clear();
  anchor_ = {};
  selected_ = 0;
  firstVisible_ = 0;
  active_ = false;
  return true;
}

Layout Model::layout(const Bounds& bounds) {
  Layout result;
  if (!active_ || labels_.empty() || bounds.width < 3 || bounds.height < 3) {
    return result;
  }

  const int topInset =
      std::clamp(bounds.topInset, 0, std::max(0, bounds.height - 1));
  const int availableHeight = bounds.height - topInset;
  if (availableHeight < 3) {
    return result;
  }

  int itemWidth = 0;
  for (const std::string& label : labels_) {
    itemWidth = std::max(itemWidth, utf8DisplayWidth(label));
  }
  result.width = std::min(bounds.width, std::max(18, itemWidth + 4));
  result.visibleRows = std::min(
      static_cast<int>(labels_.size()), std::max(1, availableHeight - 2));
  result.height = result.visibleRows + 2;

  ensureSelectionVisible(result.visibleRows);
  result.firstItem = firstVisible_;

  int x = anchor_.x;
  int y = anchor_.y;
  if (x < 0 || y < 0) {
    x = (bounds.width - result.width) / 2;
    y = topInset + (availableHeight - result.height) / 2;
  }
  result.x = std::clamp(x, 0, std::max(0, bounds.width - result.width));
  result.y =
      std::clamp(y, topInset, std::max(topInset, bounds.height - result.height));
  result.listY = result.y + 1;
  result.valid = true;
  return result;
}

Interaction Model::handle(const InputEvent& event, const Bounds& bounds) {
  Interaction result;
  if (!active_) {
    return result;
  }

  if (event.type == InputEvent::Type::Action &&
      event.action == InputAction::Back) {
    result.consumed = true;
    result.changed = dismiss();
    result.dismissed = true;
    return result;
  }

  if (event.type == InputEvent::Type::Key) {
    result.consumed = true;
    const int repetitions = static_cast<int>(std::min<std::size_t>(
        keyPressCount(event.key), std::max<std::size_t>(1, labels_.size())));
    if (event.key.vk == VK_ESCAPE) {
      if (!isAutoRepeat(event.key)) {
        result.changed = dismiss();
        result.dismissed = true;
      }
    } else if (event.key.vk == VK_UP) {
      moveSelection(-repetitions, bounds);
      result.changed = true;
    } else if (event.key.vk == VK_DOWN) {
      moveSelection(repetitions, bounds);
      result.changed = true;
    } else if (event.key.vk == VK_RETURN && !isAutoRepeat(event.key)) {
      return activateSelected();
    }
    return result;
  }

  if (event.type != InputEvent::Type::Mouse) {
    return result;
  }

  result.consumed = true;
  const Layout currentLayout = layout(bounds);
  const MouseEvent& mouse = event.mouse;
  if (mouse.kind == MouseEventKind::VerticalWheel) {
    if (mouse.wheelDelta != 0) {
      moveSelection(mouse.wheelDelta > 0 ? -1 : 1, bounds);
      result.changed = true;
    }
    return result;
  }

  const bool pointerInsideRows =
      currentLayout.valid && mouse.pos.X >= currentLayout.x &&
      mouse.pos.X < currentLayout.x + currentLayout.width &&
      mouse.pos.Y >= currentLayout.listY &&
      mouse.pos.Y < currentLayout.listY + currentLayout.visibleRows;
  if (mouse.kind == MouseEventKind::Move) {
    if (pointerInsideRows) {
      const int item = currentLayout.firstItem +
                       (mouse.pos.Y - currentLayout.listY);
      if (item >= 0 && item < static_cast<int>(labels_.size()) &&
          selected_ != item) {
        selected_ = item;
        result.changed = true;
      }
    }
    return result;
  }

  if (mouse.kind != MouseEventKind::Press ||
      !isMouseButtonDown(mouse, MouseButton::Left)) {
    return result;
  }

  if (pointerInsideRows) {
    selected_ = currentLayout.firstItem +
                (mouse.pos.Y - currentLayout.listY);
    return activateSelected();
  }

  result.changed = dismiss();
  result.dismissed = true;
  return result;
}

void Model::moveSelection(int direction, const Bounds& bounds) {
  if (labels_.empty()) {
    selected_ = 0;
    firstVisible_ = 0;
    return;
  }
  const int itemCount = static_cast<int>(labels_.size());
  selected_ = (selected_ + direction) % itemCount;
  if (selected_ < 0) {
    selected_ += itemCount;
  }
  const Layout currentLayout = layout(bounds);
  ensureSelectionVisible(currentLayout.visibleRows);
}

void Model::ensureSelectionVisible(int visibleRows) {
  if (labels_.empty() || visibleRows <= 0) {
    firstVisible_ = 0;
    return;
  }
  selected_ = std::clamp(selected_, 0, static_cast<int>(labels_.size()) - 1);
  if (selected_ < firstVisible_) {
    firstVisible_ = selected_;
  } else if (selected_ >= firstVisible_ + visibleRows) {
    firstVisible_ = selected_ - visibleRows + 1;
  }
  firstVisible_ =
      std::clamp(firstVisible_, 0,
                 std::max(0, static_cast<int>(labels_.size()) - visibleRows));
}

Interaction Model::activateSelected() {
  Interaction result;
  result.consumed = true;
  if (labels_.empty()) {
    return result;
  }
  result.activatedItem = static_cast<std::size_t>(selected_);
  result.changed = dismiss();
  result.dismissed = true;
  return result;
}

}  // namespace tui_popup_menu
