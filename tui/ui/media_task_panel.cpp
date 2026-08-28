#include "tui/ui/media_task_panel.h"

#include <algorithm>
#include <cmath>

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

bool IndicatorLayout::contains(int pointerX, int pointerY) const {
  return valid && pointerY == y && pointerX >= x && pointerX < x + width;
}

void State::synchronize(const std::optional<MediaTaskCardModel>& task) {
  if (!task) {
    hidden_ = false;
    focused_ = false;
    selectedButton_ = 0;
    hoveredButton_.reset();
    indicatorHovered_ = false;
    return;
  }
  const std::size_t actionCount = actionsFor(*task).size();
  selectedButton_ =
      actionCount == 0 ? 0 : std::min(selectedButton_, actionCount - 1);
}

bool State::show() {
  if (!hidden_) return false;
  hidden_ = false;
  focused_ = false;
  indicatorHovered_ = false;
  selectedButton_ = 0;
  return true;
}

std::optional<std::size_t> State::highlightedButton() const {
  return focused_ ? std::optional<std::size_t>(selectedButton_)
                  : hoveredButton_;
}

void State::setFocused(bool focused, Interaction& interaction) {
  if (focused_ == focused) {
    return;
  }
  focused_ = focused;
  interaction.changed = true;
  interaction.focusChanged = true;
}

void State::activate(Action action, ActivationSource source,
                     Interaction& interaction) {
  interaction.activatedAction = action;
  switch (action) {
    case Action::Cancel:
      break;
    case Action::Hide:
      hidden_ = true;
      hoveredButton_.reset();
      indicatorHovered_ = false;
      setFocused(false, interaction);
      interaction.changed = true;
      interaction.layoutChanged = true;
      break;
    case Action::Show:
      hidden_ = false;
      indicatorHovered_ = false;
      selectedButton_ = 0;
      if (source == ActivationSource::Pointer) {
        setFocused(false, interaction);
      }
      interaction.changed = true;
      interaction.layoutChanged = true;
      break;
  }
}

Interaction State::handle(const InputEvent& event, const Bounds& bounds,
                          const IndicatorLayout& indicator,
                          const MediaTaskCardModel& task) {
  Interaction result;
  if (event.type == InputEvent::Type::PointerLeave) {
    if (hoveredButton_ || indicatorHovered_) {
      hoveredButton_.reset();
      indicatorHovered_ = false;
      result.changed = true;
    }
    return result;
  }

  const std::vector<tui_button_row::Button> actions = actionsFor(task);
  const Layout currentLayout = hidden_ ? Layout{} : layout(bounds, task);
  const std::size_t visibleActionCount = currentLayout.buttons.buttons.size();
  const bool focusable = hidden_ ? indicator.valid : visibleActionCount > 0;
  if (focused_ && !focusable) {
    setFocused(false, result);
  }
  if (event.type == InputEvent::Type::Action) {
    if (focused_) {
      result.consumed = true;
      if (event.action == InputAction::Back) {
        setFocused(false, result);
      }
    }
    return result;
  }
  if (event.type == InputEvent::Type::Key) {
    if (!focused_) {
      if (event.key.vk == VK_TAB && focusable) {
        setFocused(true, result);
        result.consumed = true;
      }
      return result;
    }

    result.consumed = true;
    switch (event.key.vk) {
      case VK_TAB:
      case VK_ESCAPE:
        setFocused(false, result);
        break;
      case VK_LEFT:
        if (!hidden_) {
          selectedButton_ = tui_button_row::selectAdjacent(
              selectedButton_, visibleActionCount, -1);
          result.changed = true;
        }
        break;
      case VK_RIGHT:
        if (!hidden_) {
          selectedButton_ = tui_button_row::selectAdjacent(
              selectedButton_, visibleActionCount, 1);
          result.changed = true;
        }
        break;
      case VK_RETURN:
        if (hidden_) {
          activate(Action::Show, ActivationSource::Keyboard, result);
        } else if (const std::optional<Action> action =
                       actionAt(actions, selectedButton_)) {
          activate(*action, ActivationSource::Keyboard, result);
        }
        break;
      default:
        break;
    }
    return result;
  }
  if (event.type != InputEvent::Type::Mouse) {
    return result;
  }

  const MouseEvent& mouse = event.mouse;
  if (hidden_) {
    const bool hovered = indicator.contains(mouse.pos.X, mouse.pos.Y);
    if (indicatorHovered_ != hovered) {
      indicatorHovered_ = hovered;
      result.changed = true;
    }
    result.consumed = hovered;
    if (!hovered && focused_ && mouse.kind == MouseEventKind::Press) {
      setFocused(false, result);
    }
    if (hovered && mouse.kind == MouseEventKind::Press &&
        isMouseButtonDown(mouse, MouseButton::Left)) {
      activate(Action::Show, ActivationSource::Pointer, result);
      result.consumed = true;
    }
    return result;
  }

  const std::optional<std::size_t> hovered =
      tui_button_row::hitTest(currentLayout.buttons, mouse.pos.X, mouse.pos.Y);
  if (hoveredButton_ != hovered) {
    hoveredButton_ = hovered;
    result.changed = true;
  }
  result.consumed = currentLayout.contains(mouse.pos.X, mouse.pos.Y);
  if (!result.consumed && focused_ && mouse.kind == MouseEventKind::Press) {
    setFocused(false, result);
  }
  if (focused_ && hovered && selectedButton_ != *hovered) {
    selectedButton_ = *hovered;
    result.changed = true;
  }
  if (mouse.kind == MouseEventKind::Press &&
      isMouseButtonDown(mouse, MouseButton::Left)) {
    if (hovered) {
      selectedButton_ = *hovered;
    }
    if (const std::optional<Action> action = actionAt(actions, hovered)) {
      activate(*action, ActivationSource::Pointer, result);
    }
    result.consumed = result.consumed || result.activatedAction.has_value();
  }
  return result;
}

std::vector<tui_button_row::Button> actionsFor(const MediaTaskCardModel& task) {
  std::vector<tui_button_row::Button> actions;
  if (task.cancellable) {
    actions.push_back(
        {static_cast<tui_button_row::ButtonId>(Action::Cancel), "Cancel"});
  }
  actions.push_back(
      {static_cast<tui_button_row::ButtonId>(Action::Hide), "Hide"});
  return actions;
}

Layout layout(const Bounds& bounds, const MediaTaskCardModel& task) {
  Layout result;
  if (bounds.width < 4 || bounds.height - bounds.top < 3) {
    return result;
  }

  const std::vector<tui_button_row::Button> actions = actionsFor(task);
  int contentWidth =
      std::max({utf8DisplayWidth(task.title), utf8DisplayWidth(task.sourceName),
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
  result.height =
      std::min(contentRows + actionRows + 2, bounds.height - bounds.top);
  result.progressY = result.y + (task.detail.empty() ? 3 : 4);
  if (!actions.empty() && result.height >= contentRows + 4) {
    result.buttons = tui_button_row::layout(actions, result.x, result.width,
                                            result.y + result.height - 2);
  }
  result.valid = result.height >= 3;
  return result;
}

std::string indicatorText(const MediaTaskCardModel& task) {
  std::string text = task.title;
  if (task.progress) {
    const int percent = static_cast<int>(
        std::round(std::clamp(*task.progress, 0.0f, 1.0f) * 100.0f));
    text += " " + std::to_string(percent) + "%";
  }
  return text;
}

IndicatorLayout indicatorLayout(int availableWidth, int y,
                                const MediaTaskCardModel& task) {
  IndicatorLayout result;
  if (availableWidth <= 0 || y < 0) return result;
  result.x = 0;
  result.y = y;
  result.width = std::min(availableWidth,
                          utf8DisplayWidth("[ " + indicatorText(task) + " ]"));
  result.valid = result.width > 0;
  return result;
}

tui_dialog::Content cancellationDialog(const MediaTaskCardModel& task) {
  tui_dialog::Content content;
  content.title = "Cancel " + task.operationName + "?";
  content.text.push_back({"Progress on " + task.sourceName + " will be lost.",
                          tui_dialog::TextTone::Normal});
  content.buttons.push_back({kCancelTaskButton, "Cancel task"});
  content.buttons.push_back({kKeepRunningButton, "Keep running"});
  content.initiallySelectedButton = kKeepRunningButton;
  return content;
}

}  // namespace tui_media_task_panel
