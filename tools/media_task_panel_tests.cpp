#include <cstdlib>
#include <iostream>

#include "tui/ui/media_task_panel.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "media_task_panel_tests: " << message << '\n';
  return false;
}

InputEvent pointerEvent(MouseEventKind kind, int x, int y,
                        bool leftButtonDown = false) {
  InputEvent event;
  event.type = InputEvent::Type::Mouse;
  event.mouse.kind = kind;
  event.mouse.pos.X = static_cast<SHORT>(x);
  event.mouse.pos.Y = static_cast<SHORT>(y);
  event.mouse.button =
      kind == MouseEventKind::Press ? MouseButton::Left : MouseButton::None;
  event.mouse.buttons =
      leftButtonDown ? MouseButtons::Left : MouseButtons::None;
  return event;
}

InputEvent keyEvent(WORD key) {
  InputEvent event;
  event.type = InputEvent::Type::Key;
  event.key.vk = key;
  return event;
}

}  // namespace

int main() {
  using namespace tui_media_task_panel;
  bool ok = true;

  MediaTaskCardModel task;
  task.title = "Separating audio";
  task.operationName = "audio separation";
  task.sourceName = "NTE.mp4";
  task.detail = "Separating dialogue, music and effects on GPU";
  task.progress = 0.10f;
  task.cancellable = true;

  const Bounds bounds{100, 30, 2};
  const IndicatorLayout noIndicator;
  const Layout card = layout(bounds, task);
  ok &=
      expect(card.valid && card.width >= 46 && card.buttons.buttons.size() == 2,
             "an active task must expose direct Cancel and Hide buttons");

  State state;
  const tui_button_row::Placement& cancel = card.buttons.buttons.front();
  Interaction interaction =
      state.handle(pointerEvent(MouseEventKind::Move, cancel.x, card.buttons.y),
                   bounds, noIndicator, task);
  ok &= expect(
      interaction.consumed && interaction.changed && state.hoveredButton() == 0,
      "hovering the task button must be visible and consume the "
      "covered browser cell");

  interaction = state.handle(
      pointerEvent(MouseEventKind::Press, cancel.x, card.buttons.y, true),
      bounds, noIndicator, task);
  ok &= expect(
      interaction.consumed && interaction.activatedAction == Action::Cancel,
      "clicking Cancel must publish a typed panel action");

  interaction = state.handle(pointerEvent(MouseEventKind::Move, 0, 0), bounds,
                             noIndicator, task);
  ok &= expect(
      !interaction.consumed && interaction.changed && !state.hoveredButton(),
      "leaving the panel must clear hover without blocking browser "
      "input");

  interaction = state.handle(keyEvent(VK_TAB), bounds, noIndicator, task);
  ok &= expect(interaction.consumed && interaction.focusChanged &&
                   state.focused() && state.highlightedButton() == 0,
               "Tab must move focus from the browser into an actionable "
               "task panel");
  interaction = state.handle(keyEvent(VK_RETURN), bounds, noIndicator, task);
  ok &= expect(
      interaction.consumed && interaction.activatedAction == Action::Cancel,
      "Enter must activate the selected task-panel button");
  interaction = state.handle(keyEvent(VK_RIGHT), bounds, noIndicator, task);
  ok &= expect(interaction.consumed && state.highlightedButton() == 1,
               "Right must select the adjacent task-panel button");
  interaction = state.handle(keyEvent(VK_RETURN), bounds, noIndicator, task);
  ok &= expect(interaction.consumed && interaction.layoutChanged &&
                   interaction.activatedAction == Action::Hide &&
                   state.hidden() && !state.focused(),
               "Hide must return focus to the browser and replace the panel "
               "with its compact status");

  const IndicatorLayout indicator = indicatorLayout(100, 25, task);
  ok &= expect(indicator.valid && indicatorText(task) == "Separating audio 10%",
               "a hidden task must retain a compact measurable status");
  interaction = state.handle(keyEvent(VK_TAB), bounds, indicator, task);
  ok &= expect(
      interaction.consumed && state.focused() && state.indicatorHighlighted(),
      "Tab must make a hidden task indicator keyboard reachable");
  interaction = state.handle(keyEvent(VK_RETURN), bounds, indicator, task);
  ok &= expect(interaction.consumed && interaction.layoutChanged &&
                   interaction.activatedAction == Action::Show &&
                   !state.hidden() && state.focused(),
               "Enter on the indicator must restore the focused panel");
  interaction = state.handle(keyEvent(VK_ESCAPE), bounds, noIndicator, task);
  ok &= expect(
      interaction.consumed && interaction.focusChanged && !state.focused(),
      "Escape must return task-panel focus to the browser");

  state.handle(keyEvent(VK_TAB), bounds, noIndicator, task);
  state.handle(keyEvent(VK_RIGHT), bounds, noIndicator, task);
  state.handle(keyEvent(VK_RETURN), bounds, noIndicator, task);
  interaction =
      state.handle(pointerEvent(MouseEventKind::Move, indicator.x, indicator.y),
                   bounds, indicator, task);
  ok &= expect(interaction.consumed && interaction.changed &&
                   state.indicatorHighlighted(),
               "the hidden-task indicator must expose pointer hover");
  interaction = state.handle(
      pointerEvent(MouseEventKind::Press, indicator.x, indicator.y, true),
      bounds, indicator, task);
  ok &= expect(interaction.consumed && interaction.layoutChanged &&
                   interaction.activatedAction == Action::Show &&
                   !state.hidden() && !state.focused(),
               "clicking the footer indicator must restore the panel without "
               "stealing browser focus");

  state.handle(keyEvent(VK_TAB), bounds, noIndicator, task);
  interaction = state.handle(
      pointerEvent(MouseEventKind::Press, 0, bounds.top + 10, true), bounds,
      noIndicator, task);
  ok &= expect(
      !interaction.consumed && interaction.focusChanged && !state.focused(),
      "clicking outside a focused panel must return focus and let "
      "the browser receive the click");

  const tui_dialog::Content confirmation = cancellationDialog(task);
  ok &= expect(confirmation.title == "Cancel audio separation?" &&
                   confirmation.buttons.size() == 2 &&
                   confirmation.buttons[0].id == kCancelTaskButton &&
                   confirmation.initiallySelectedButton == kKeepRunningButton,
               "cancellation must require a safe-default confirmation");

  task.cancellable = false;
  const std::vector<tui_button_row::Button> nonCancellableActions =
      actionsFor(task);
  ok &= expect(nonCancellableActions.size() == 1 &&
                   nonCancellableActions.front().id ==
                       static_cast<tui_button_row::ButtonId>(Action::Hide),
               "a non-cancellable task must retain Hide without exposing a "
               "fake Cancel action");

  state.handle(keyEvent(VK_TAB), bounds, noIndicator, task);
  state.handle(keyEvent(VK_RETURN), bounds, noIndicator, task);
  ok &= expect(state.hidden(), "the remaining Hide action must stay usable");
  state.synchronize(std::nullopt);
  ok &= expect(!state.hidden() && !state.focused(),
               "task completion must reset presentation state for the next "
               "background task");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
