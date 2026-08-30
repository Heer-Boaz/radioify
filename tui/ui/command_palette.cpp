#include "command_palette.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "single_line_text_input.h"

namespace tui_command_palette {
namespace {

std::string lowercaseAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

bool fuzzyMatch(const std::string& text, const std::string& query) {
  if (query.empty()) {
    return true;
  }
  std::size_t textIndex = 0;
  for (const char queryCharacter : query) {
    textIndex = text.find(queryCharacter, textIndex);
    if (textIndex == std::string::npos) {
      return false;
    }
    ++textIndex;
  }
  return true;
}

}  // namespace

Command::Command(std::string label, std::string hotkey)
    : label_(std::move(label)),
      hotkey_(std::move(hotkey)) {}

void Model::open() {
  query_.clear();
  filteredCommandIndices_.clear();
  resetSelection();
  active_ = true;
}

bool Model::dismiss() {
  if (!active_ && query_.empty() && filteredCommandIndices_.empty()) {
    return false;
  }
  query_.clear();
  filteredCommandIndices_.clear();
  resetSelection();
  active_ = false;
  return true;
}

Layout Model::layout(const std::vector<Command>& commands,
                     const Bounds& bounds) {
  Layout result;
  if (!active_) {
    return result;
  }

  refreshFilter(commands);
  if (bounds.width < 3 || bounds.height < 4) {
    return result;
  }
  const int topInset =
      std::clamp(bounds.topInset, 0, std::max(0, bounds.height - 1));
  const int availableHeight = bounds.height - topInset;
  if (availableHeight < 4) {
    return result;
  }

  const int desiredWidth = std::min(72, std::max(30, bounds.width - 4));
  result.width = std::min(bounds.width, desiredWidth);
  result.innerWidth = std::max(1, result.width - 2);
  const int maximumRows = std::max(1, availableHeight - 3);
  result.visibleRows =
      std::min(maximumRows,
               std::max(1, static_cast<int>(filteredCommandIndices_.size())));
  result.height = result.visibleRows + 3;
  result.x = std::max(0, (bounds.width - result.width) / 2);
  result.y = topInset + std::max(0, (availableHeight - result.height) / 2);
  result.inputY = result.y + 1;
  result.listY = result.y + 2;

  ensureSelectionVisible(result.visibleRows);
  result.firstFilteredItem = firstVisible_;
  result.valid = true;
  return result;
}

Interaction Model::handle(const InputEvent& event,
                          const std::vector<Command>& commands,
                          const Bounds& bounds) {
  Interaction result;
  if (!active_) {
    return result;
  }

  Layout currentLayout = layout(commands, bounds);
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
        keyPressCount(event.key),
        std::max<std::size_t>(1, filteredCommandIndices_.size())));
    if (event.key.vk == VK_UP) {
      moveSelection(-repetitions, currentLayout.visibleRows);
      result.changed = true;
    } else if (event.key.vk == VK_DOWN) {
      moveSelection(repetitions, currentLayout.visibleRows);
      result.changed = true;
    } else {
      const single_line_text_input::EditResult edit =
          single_line_text_input::edit(query_, event.key);
      if (edit.intent == single_line_text_input::Intent::Cancel) {
        result.changed = dismiss();
        result.dismissed = true;
      } else if (edit.intent == single_line_text_input::Intent::Commit) {
        return activateSelected();
      } else if (edit.changed) {
        resetSelection();
        refreshFilter(commands);
        result.changed = true;
      }
    }
    return result;
  }

  if (event.type != InputEvent::Type::Mouse) {
    return result;
  }

  result.consumed = true;
  const MouseEvent& mouse = event.mouse;
  if (mouse.kind == MouseEventKind::VerticalWheel) {
    if (mouse.wheelDelta != 0) {
      moveSelection(mouse.wheelDelta > 0 ? -1 : 1,
                    currentLayout.visibleRows);
      result.changed = true;
    }
    return result;
  }

  const bool pointerInsideRows =
      currentLayout.valid && mouse.pos.X >= currentLayout.x + 1 &&
      mouse.pos.X < currentLayout.x + currentLayout.width - 1 &&
      mouse.pos.Y >= currentLayout.listY &&
      mouse.pos.Y < currentLayout.listY + currentLayout.visibleRows;
  if (mouse.kind == MouseEventKind::Move) {
    if (pointerInsideRows && !filteredCommandIndices_.empty()) {
      const int filteredItem = currentLayout.firstFilteredItem +
                               (mouse.pos.Y - currentLayout.listY);
      if (filteredItem >= 0 &&
          filteredItem < static_cast<int>(filteredCommandIndices_.size()) &&
          selected_ != filteredItem) {
        selected_ = filteredItem;
        result.changed = true;
      }
    }
    return result;
  }

  if (mouse.kind != MouseEventKind::Press ||
      !isMouseButtonDown(mouse, MouseButton::Left)) {
    return result;
  }

  if (pointerInsideRows && !filteredCommandIndices_.empty()) {
    selected_ = currentLayout.firstFilteredItem +
                (mouse.pos.Y - currentLayout.listY);
    return activateSelected();
  }

  result.changed = dismiss();
  result.dismissed = true;
  return result;
}

void Model::refreshFilter(const std::vector<Command>& commands) {
  filteredCommandIndices_.clear();
  const std::string lowercaseQuery = lowercaseAscii(query_);
  for (std::size_t index = 0; index < commands.size(); ++index) {
    if (fuzzyMatch(lowercaseAscii(commands[index].label()), lowercaseQuery)) {
      filteredCommandIndices_.push_back(index);
    }
  }
  if (filteredCommandIndices_.empty()) {
    resetSelection();
  } else {
    selected_ = std::clamp(
        selected_, 0, static_cast<int>(filteredCommandIndices_.size()) - 1);
  }
}

void Model::resetSelection() {
  selected_ = 0;
  firstVisible_ = 0;
}

void Model::moveSelection(int offset, int visibleRows) {
  if (filteredCommandIndices_.empty()) {
    resetSelection();
    return;
  }
  selected_ =
      std::clamp(selected_ + offset, 0,
                 static_cast<int>(filteredCommandIndices_.size()) - 1);
  ensureSelectionVisible(visibleRows);
}

void Model::ensureSelectionVisible(int visibleRows) {
  if (filteredCommandIndices_.empty() || visibleRows <= 0) {
    resetSelection();
    return;
  }
  selected_ = std::clamp(
      selected_, 0, static_cast<int>(filteredCommandIndices_.size()) - 1);
  if (selected_ < firstVisible_) {
    firstVisible_ = selected_;
  } else if (selected_ >= firstVisible_ + visibleRows) {
    firstVisible_ = selected_ - visibleRows + 1;
  }
  firstVisible_ = std::clamp(
      firstVisible_, 0,
      std::max(0, static_cast<int>(filteredCommandIndices_.size()) -
                      visibleRows));
}

Interaction Model::activateSelected() {
  Interaction result;
  result.consumed = true;
  if (filteredCommandIndices_.empty()) {
    return result;
  }
  result.activatedCommand = filteredCommandIndices_[static_cast<std::size_t>(
      std::clamp(selected_, 0,
                 static_cast<int>(filteredCommandIndices_.size()) - 1))];
  result.changed = dismiss();
  result.dismissed = true;
  return result;
}

}  // namespace tui_command_palette
