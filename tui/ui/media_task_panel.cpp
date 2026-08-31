#include "tui/ui/media_task_panel.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/unicode_display_width.h"
#include "playback/media_action_confirmation_content.h"

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

std::string taskProgressText(const MediaTaskCardModel& task) {
  std::string text = task.title;
  if (task.progress) {
    const int percent = static_cast<int>(
        std::round(std::clamp(*task.progress, 0.0f, 1.0f) * 100.0f));
    text += " " + std::to_string(percent) + "%";
  }
  return text;
}

void clearDialogSession(std::optional<tui_dialog::DialogId>& dialog,
                        std::optional<DialogContext>& context) {
  dialog.reset();
  context.reset();
}

}  // namespace

void DialogSession::opened(tui_dialog::DialogId dialog, DialogContext context) {
  dialog_ = dialog;
  context_ = std::move(context);
}

std::optional<tui_dialog::DialogId> DialogSession::synchronize(
    const std::optional<MediaTaskCardModel>& activeTask,
    const std::optional<MediaTaskFailureDialogModel>& latestFailure,
    const AudioSeparationSetupRequired& setupRequired) {
  if (!dialog_ || !context_) {
    return std::nullopt;
  }

  bool current = false;
  switch (context_->kind) {
    case DialogKind::Cancellation:
      current = activeTask && activeTask->taskId == context_->taskId &&
                activeTask->cancellable;
      break;
    case DialogKind::Failure:
      current = !activeTask && latestFailure &&
                latestFailure->taskId == context_->taskId;
      break;
    case DialogKind::AudioSeparationSetup:
      current = !activeTask && setupRequired &&
                setupRequired(context_->sourceFile);
      break;
  }
  if (current) {
    return std::nullopt;
  }
  const tui_dialog::DialogId obsolete = *dialog_;
  clearDialogSession(dialog_, context_);
  return obsolete;
}

std::optional<DialogIntent> DialogSession::handle(
    const tui_dialog::ButtonActivation& activation) {
  if (!dialog_ || !context_ || activation.dialog != *dialog_) {
    return std::nullopt;
  }

  const DialogContext context = *context_;
  clearDialogSession(dialog_, context_);
  if (context.kind == DialogKind::Cancellation &&
      activation.button == kCancelTaskButton) {
    return DialogIntent{CancelTask{context.taskId}};
  }
  if (context.kind == DialogKind::Failure &&
      activation.button == kMediaTaskDialogRetry && context.retryAction) {
    return DialogIntent{
        RetryTask{context.taskId, context.sourceFile, *context.retryAction}};
  }
  if (context.kind == DialogKind::AudioSeparationSetup &&
      activation.button == kSetUpAudioButton) {
    return DialogIntent{SetUpAudioSeparation{
        playback_media_processing::AudioSeparationSetupRequest{
            context.sourceFile}}};
  }
  return std::nullopt;
}

void DialogSession::dismissed(tui_dialog::DialogId dialog) {
  if (dialog_ && *dialog_ == dialog) {
    clearDialogSession(dialog_, context_);
  }
}

void DeferredFailureState::observe(
    std::optional<MediaTaskFailureDialogModel> completedTaskFailure,
    const std::optional<MediaTaskCardModel>& activeTask) {
  if (activeTask) {
    pending_.reset();
    return;
  }
  pending_ = std::move(completedTaskFailure);
}

void DeferredFailureState::synchronize(
    const std::optional<MediaTaskCardModel>& activeTask) {
  if (activeTask) {
    pending_.reset();
  }
}

std::optional<MediaTaskFailureDialogModel> DeferredFailureState::take() {
  std::optional<MediaTaskFailureDialogModel> result = std::move(pending_);
  pending_.reset();
  return result;
}

bool Layout::contains(int pointerX, int pointerY) const {
  return valid && pointerX >= x && pointerX < x + width && pointerY >= y &&
         pointerY < y + height;
}

bool IndicatorLayout::contains(int pointerX, int pointerY) const {
  return actionVisible && pointerY == y && pointerX >= showX &&
         pointerX < showX + showWidth;
}

void State::synchronize(const std::optional<MediaTaskCardModel>& task) {
  const std::optional<media_processing::TaskId> nextTask =
      task ? std::optional<media_processing::TaskId>(task->taskId)
           : std::nullopt;
  if (taskId_ != nextTask) {
    hidden_ = false;
    focused_ = false;
    selectedButton_ = 0;
    buttonPointer_.reset();
    actionIds_.clear();
    taskId_ = nextTask;
  }
  if (!task) {
    return;
  }

  const std::vector<tui_button_row::Button> actions = actionsFor(*task);
  std::vector<tui_button_row::ButtonId> nextActionIds;
  nextActionIds.reserve(actions.size());
  for (const tui_button_row::Button& action : actions) {
    nextActionIds.push_back(action.id);
  }
  if (actionIds_ != nextActionIds) {
    buttonPointer_.reset();
    actionIds_ = std::move(nextActionIds);
  }
  const std::size_t actionCount = actions.size();
  selectedButton_ =
      actionCount == 0 ? 0 : std::min(selectedButton_, actionCount - 1);
}

bool State::show() {
  if (!hidden_) return false;
  hidden_ = false;
  focused_ = false;
  buttonPointer_.reset();
  selectedButton_ = 0;
  return true;
}

std::optional<std::size_t> State::highlightedButton() const {
  return focused_ ? std::optional<std::size_t>(selectedButton_)
                  : buttonPointer_.hovered();
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
      buttonPointer_.reset();
      setFocused(false, interaction);
      interaction.changed = true;
      interaction.layoutChanged = true;
      break;
    case Action::Show:
      hidden_ = false;
      buttonPointer_.reset();
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
    const tui_button_row::PointerInteraction pointer =
        buttonPointer_.handle(event, {});
    result.changed = pointer.changed;
    result.consumed = pointer.captured;
    return result;
  }

  const std::vector<tui_button_row::Button> actions = actionsFor(task);
  const Layout currentLayout = hidden_ ? Layout{} : layout(bounds, task);
  const std::size_t visibleActionCount = currentLayout.buttons.buttons.size();
  const bool focusable =
      hidden_ ? indicator.actionVisible : visibleActionCount > 0;
  if (focused_ && !focusable) {
    setFocused(false, result);
  }
  if (event.type == InputEvent::Type::Action) {
    if (focused_) {
      if (event.action == InputAction::Back) {
        result.consumed = true;
        setFocused(false, result);
      } else {
        setFocused(false, result);
      }
    }
    return result;
  }
  if (event.type == InputEvent::Type::Key) {
    const tui_button_row::KeyboardAction keyboardAction =
        tui_button_row::resolveKeyboardAction(event.key);
    if (!focused_) {
      if ((keyboardAction == tui_button_row::KeyboardAction::FocusNext ||
           keyboardAction ==
               tui_button_row::KeyboardAction::FocusPrevious) &&
          focusable) {
        if (!hidden_ &&
            keyboardAction ==
                tui_button_row::KeyboardAction::FocusPrevious) {
          selectedButton_ = visibleActionCount - 1;
        }
        setFocused(true, result);
        result.consumed = true;
      }
      return result;
    }

    switch (keyboardAction) {
      case tui_button_row::KeyboardAction::FocusPrevious:
      case tui_button_row::KeyboardAction::FocusNext:
      case tui_button_row::KeyboardAction::Dismiss:
        result.consumed = true;
        setFocused(false, result);
        break;
      case tui_button_row::KeyboardAction::SelectPrevious:
        result.consumed = true;
        if (!hidden_) {
          selectedButton_ = tui_button_row::selectAdjacent(
              selectedButton_, visibleActionCount,
              -static_cast<int>(std::min<std::size_t>(
                  keyPressCount(event.key), visibleActionCount)));
          result.changed = true;
        }
        break;
      case tui_button_row::KeyboardAction::SelectNext:
        result.consumed = true;
        if (!hidden_) {
          selectedButton_ = tui_button_row::selectAdjacent(
              selectedButton_, visibleActionCount,
              static_cast<int>(std::min<std::size_t>(
                  keyPressCount(event.key), visibleActionCount)));
          result.changed = true;
        }
        break;
      case tui_button_row::KeyboardAction::Activate:
        result.consumed = true;
        if (hidden_) {
          activate(Action::Show, ActivationSource::Keyboard, result);
        } else if (const std::optional<Action> action =
                       actionAt(actions, selectedButton_)) {
          activate(*action, ActivationSource::Keyboard, result);
        }
        break;
      case tui_button_row::KeyboardAction::SuppressRepeat:
        result.consumed = true;
        break;
      case tui_button_row::KeyboardAction::None:
        // This is a modeless panel. An unrelated key transfers focus back to
        // the browser and continues through the shell router.
        setFocused(false, result);
        break;
    }
    return result;
  }
  if (event.type != InputEvent::Type::Mouse) {
    return result;
  }

  const MouseEvent& mouse = event.mouse;
  if (hidden_) {
    tui_button_row::Layout indicatorButtons;
    indicatorButtons.y = indicator.y;
    if (indicator.actionVisible) {
      indicatorButtons.buttons.push_back(
          {0, indicator.showX, indicator.showWidth});
    }
    const tui_button_row::PointerInteraction pointer =
        buttonPointer_.handle(event, indicatorButtons);
    const bool hovered = buttonPointer_.hovered().has_value();
    result.changed = pointer.changed;
    result.consumed = hovered;
    if (!hovered && focused_ && mouse.kind == MouseEventKind::Press) {
      setFocused(false, result);
    }
    if (pointer.activated) {
      activate(Action::Show, ActivationSource::Pointer, result);
    }
    result.consumed = result.consumed || pointer.captured ||
                      result.activatedAction.has_value();
    return result;
  }

  const tui_button_row::PointerInteraction pointer =
      buttonPointer_.handle(event, currentLayout.buttons);
  const std::optional<std::size_t> hovered = buttonPointer_.hovered();
  result.changed = pointer.changed;
  result.consumed = currentLayout.contains(mouse.pos.X, mouse.pos.Y);
  if (!result.consumed && focused_ && mouse.kind == MouseEventKind::Press) {
    setFocused(false, result);
  }
  if (focused_ && hovered && selectedButton_ != *hovered) {
    selectedButton_ = *hovered;
    result.changed = true;
  }
  if (pointer.activated) {
    selectedButton_ = *pointer.activated;
    if (const std::optional<Action> action =
            actionAt(actions, pointer.activated)) {
      activate(*action, ActivationSource::Pointer, result);
    }
  }
  result.consumed =
      result.consumed || pointer.captured || result.activatedAction.has_value();
  return result;
}

std::vector<tui_button_row::Button> actionsFor(const MediaTaskCardModel& task) {
  std::vector<tui_button_row::Button> actions;
  if (task.cancellable) {
    actions.push_back({static_cast<tui_button_row::ButtonId>(Action::Cancel),
                       "Cancel", "Stop"});
  }
  actions.push_back(
      {static_cast<tui_button_row::ButtonId>(Action::Hide), "Hide", "Hide"});
  return actions;
}

Layout layout(const Bounds& bounds, const MediaTaskCardModel& task) {
  Layout result;
  if (bounds.width < 4 || bounds.height < 3) {
    return result;
  }

  const std::vector<tui_button_row::Button> actions = actionsFor(task);
  const int requestedTop =
      std::clamp(bounds.top, 0, std::max(0, bounds.height - 1));
  const int minimumActionableHeight = actions.empty() ? 3 : 4;
  // Active work and its direct actions outrank persistent browser chrome on
  // an extremely short terminal. The panel remains modeless and Hide still
  // returns the available surface to browser content.
  result.y = std::min(
      requestedTop, std::max(0, bounds.height - minimumActionableHeight));
  const int availableHeight = bounds.height - result.y;
  if (availableHeight < 3) {
    return result;
  }

  int contentWidth =
      std::max({utf8DisplayWidth(task.title), utf8DisplayWidth(task.sourceName),
                utf8DisplayWidth(task.detail)});
  if (!task.engineName.empty()) {
    contentWidth = std::max(
        contentWidth, utf8DisplayWidth("Engine: " + task.engineName));
  }
  for (const tui_button_row::Button& action : actions) {
    contentWidth = std::max(contentWidth, utf8DisplayWidth(action.label) + 4);
  }
  const int desiredWidth = std::max(46, contentWidth + 4);
  result.width = std::clamp(desiredWidth, 4, bounds.width);
  result.innerWidth = std::max(1, result.width - 2);
  result.x = std::max(0, bounds.width - result.width - 1);

  int preferredContentRows = 3;
  if (!task.engineName.empty()) {
    ++preferredContentRows;
  }
  if (!task.detail.empty()) {
    ++preferredContentRows;
  }
  const int maximumButtonRows = std::max(0, availableHeight - 3);
  if (!actions.empty() && maximumButtonRows > 0) {
    result.buttons = tui_button_row::responsiveLayout(
        actions, result.x, result.width, 0, maximumButtonRows);
  }
  const int actionRows = result.buttons.rowCount;
  const int preferredGapRows = actionRows > 0 ? 1 : 0;
  const int desiredHeight =
      preferredContentRows + actionRows + preferredGapRows + 2;
  result.height = std::min(desiredHeight, availableHeight);
  const int interiorRows = std::max(0, result.height - 2);
  result.contentRows = std::min(
      preferredContentRows, std::max(0, interiorRows - actionRows));
  const int buttonBottomY = result.y + result.height - 2;
  if (actionRows > 0) {
    result.buttons = tui_button_row::responsiveLayout(
        actions, result.x, result.width, buttonBottomY, actionRows);
  }
  result.progressY = result.y + std::max(1, result.contentRows);
  result.valid = result.height >= 3;
  return result;
}

std::string indicatorText(const MediaTaskCardModel& task) {
  return "Background task: " + task.sourceName + " - " +
         taskProgressText(task);
}

IndicatorLayout indicatorLayout(int availableWidth, int y,
                                const MediaTaskCardModel& task) {
  IndicatorLayout result;
  if (availableWidth <= 0 || y < 0) return result;
  constexpr int kShowWidth = 8;
  constexpr int kStatusGap = 2;
  result.x = 0;
  result.y = y;
  const std::string status = indicatorText(task);
  if (availableWidth >= kShowWidth) {
    const int maximumStatusWidth =
        std::max(0, availableWidth - kShowWidth - kStatusGap);
    if (maximumStatusWidth > 0) {
      const std::string compactStatus =
          task.sourceName + " - " + taskProgressText(task);
      const std::string& preferredStatus =
          utf8DisplayWidth(status) <= maximumStatusWidth ? status
                                                         : compactStatus;
      result.statusText =
          utf8TakeDisplayWidth(preferredStatus, maximumStatusWidth);
      result.statusWidth = utf8DisplayWidth(result.statusText);
    }
    result.showX = result.statusWidth > 0
                       ? result.statusWidth + kStatusGap
                       : 0;
    result.showWidth = kShowWidth;
    result.actionVisible = true;
    result.width = result.showX + result.showWidth;
  } else {
    result.statusText = utf8TakeDisplayWidth(status, availableWidth);
    result.statusWidth = utf8DisplayWidth(result.statusText);
    result.width = result.statusWidth;
  }
  result.valid = result.width > 0;
  return result;
}

DialogRequest cancellationDialogRequest(const MediaTaskCardModel& task) {
  DialogRequest request;
  request.context.kind = DialogKind::Cancellation;
  request.context.taskId = task.taskId;
  const playback_media_confirmation::Content content =
      playback_media_confirmation::cancellationContent(task.operation,
                                                       task.sourceName);
  request.content.title = content.title;
  for (const std::string& line : content.text) {
    request.content.text.push_back({line, tui_dialog::TextTone::Normal});
  }
  request.content.buttons.push_back(
      {kCancelTaskButton, content.primaryLabel, "Stop"});
  request.content.buttons.push_back(
      {kKeepRunningButton, content.secondaryLabel, "Keep"});
  request.content.initiallySelectedButton = kKeepRunningButton;
  return request;
}

DialogRequest audioSeparationSetupDialogRequest(
    playback_media_processing::AudioSeparationSetupRequest setup) {
  DialogRequest request;
  request.context.kind = DialogKind::AudioSeparationSetup;
  request.context.sourceFile = std::move(setup.sourceFile);
  const playback_media_confirmation::Content content =
      playback_media_confirmation::audioSeparationSetupContent();
  for (std::size_t index = 0; index < content.text.size(); ++index) {
    request.content.text.push_back(
        {content.text[index], index == 0 ? tui_dialog::TextTone::Normal
                                        : tui_dialog::TextTone::Secondary});
  }
  request.content.title = content.title;
  request.content.buttons.push_back(
      {kSetUpAudioButton, content.primaryLabel, "Install"});
  request.content.buttons.push_back(
      {kNotNowButton, content.secondaryLabel, "Cancel"});
  request.content.initiallySelectedButton = kNotNowButton;
  return request;
}

DialogRequest failureDialogRequest(const MediaTaskFailureDialogModel& failure) {
  DialogRequest request;
  request.content = failure.content;
  request.context.kind = DialogKind::Failure;
  request.context.taskId = failure.taskId;
  request.context.sourceFile = failure.sourceFile;
  request.context.retryAction = failure.retryAction;
  return request;
}

}  // namespace tui_media_task_panel
