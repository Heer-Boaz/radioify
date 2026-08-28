#include "tui/ui/media_task_panel.h"

#include <algorithm>

#include "core/unicode_display_width.h"

namespace tui_media_task_panel {
namespace {

std::optional<Action> actionAt(
    const std::vector<tui_button_row::Button>& actions,
    std::optional<std::size_t> index) {
  if (!index || *index >= actions.size()) {
    return std::nullopt;
  }
  return static_cast<Action>(actions[*index].id);
}

}  // namespace

bool Layout::contains(int pointerX, int pointerY) const {
  return valid && pointerX >= x && pointerX < x + width && pointerY >= y &&
         pointerY < y + height;
}

void State::synchronize(bool taskActive) {
  if (!taskActive) {
    hoveredButton_.reset();
  }
}

Interaction State::handle(const InputEvent& event, const Bounds& bounds,
                          const MediaTaskCardModel& task) {
  Interaction result;
  if (event.type == InputEvent::Type::PointerLeave) {
    if (hoveredButton_) {
      hoveredButton_.reset();
      result.changed = true;
    }
    return result;
  }
  if (event.type != InputEvent::Type::Mouse) {
    return result;
  }

  const Layout currentLayout = layout(bounds, task);
  const MouseEvent& mouse = event.mouse;
  const std::vector<tui_button_row::Button> actions = actionsFor(task);
  const std::optional<std::size_t> hovered =
      tui_button_row::hitTest(currentLayout.buttons, mouse.pos.X,
                              mouse.pos.Y);
  if (hoveredButton_ != hovered) {
    hoveredButton_ = hovered;
    result.changed = true;
  }
  result.consumed = currentLayout.contains(mouse.pos.X, mouse.pos.Y);
  if (mouse.kind == MouseEventKind::Press &&
      isMouseButtonDown(mouse, MouseButton::Left)) {
    result.activatedAction = actionAt(actions, hovered);
    result.consumed = result.consumed || result.activatedAction.has_value();
  }
  return result;
}

std::vector<tui_button_row::Button> actionsFor(
    const MediaTaskCardModel& task) {
  std::vector<tui_button_row::Button> actions;
  if (task.cancellable) {
    actions.push_back(
        {static_cast<tui_button_row::ButtonId>(Action::Cancel), "Cancel"});
  }
  return actions;
}

Layout layout(const Bounds& bounds, const MediaTaskCardModel& task) {
  Layout result;
  if (bounds.width < 4 || bounds.height - bounds.top < 3) {
    return result;
  }

  const std::vector<tui_button_row::Button> actions = actionsFor(task);
  int contentWidth = std::max(
      {utf8DisplayWidth(task.title), utf8DisplayWidth(task.sourceName),
       utf8DisplayWidth(task.detail)});
  for (const tui_button_row::Button& action : actions) {
    contentWidth = std::max(contentWidth, utf8DisplayWidth(action.label) + 4);
  }
  const int desiredWidth = std::max(46, contentWidth + 4);
  result.width = std::clamp(desiredWidth, 4, bounds.width);
  result.innerWidth = std::max(1, result.width - 2);
  result.x = std::max(0, bounds.width - result.width - 1);
  result.y = bounds.top;

  int contentRows = 3;
  if (!task.detail.empty()) {
    ++contentRows;
  }
  const int actionRows = actions.empty() ? 0 : 2;
  result.height = std::min(contentRows + actionRows + 2,
                           bounds.height - bounds.top);
  result.progressY = result.y + (task.detail.empty() ? 3 : 4);
  if (!actions.empty() && result.height >= contentRows + 4) {
    result.buttons = tui_button_row::layout(
        actions, result.x, result.width, result.y + result.height - 2);
  }
  result.valid = result.height >= 3;
  return result;
}

tui_dialog::Content cancellationDialog(const MediaTaskCardModel& task) {
  tui_dialog::Content content;
  content.title = "Cancel " + task.operationName + "?";
  content.text.push_back(
      {"Progress on " + task.sourceName + " will be lost.",
       tui_dialog::TextTone::Normal});
  content.buttons.push_back({kCancelTaskButton, "Cancel task"});
  content.buttons.push_back({kKeepRunningButton, "Keep running"});
  content.initiallySelectedButton = kKeepRunningButton;
  return content;
}

}  // namespace tui_media_task_panel
