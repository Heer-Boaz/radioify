#include "tui/ui/media_task_panel.h"

#include <cstdlib>
#include <iostream>

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
  const Layout card = layout(bounds, task);
  ok &= expect(card.valid && card.width >= 46 &&
                   card.buttons.buttons.size() == 1,
               "a cancellable task must expose one direct Cancel button");

  State state;
  const tui_button_row::Placement& cancel = card.buttons.buttons.front();
  Interaction interaction = state.handle(
      pointerEvent(MouseEventKind::Move, cancel.x, card.buttons.y), bounds,
      task);
  ok &= expect(interaction.consumed && interaction.changed &&
                   state.hoveredButton() == 0,
               "hovering the task button must be visible and consume the "
               "covered browser cell");

  interaction = state.handle(
      pointerEvent(MouseEventKind::Press, cancel.x, card.buttons.y, true),
      bounds, task);
  ok &= expect(interaction.consumed &&
                   interaction.activatedAction == Action::Cancel,
               "clicking Cancel must publish a typed panel action");

  interaction = state.handle(pointerEvent(MouseEventKind::Move, 0, 0),
                             bounds, task);
  ok &= expect(!interaction.consumed && interaction.changed &&
                   !state.hoveredButton(),
               "leaving the panel must clear hover without blocking browser "
               "input");

  const tui_dialog::Content confirmation = cancellationDialog(task);
  ok &= expect(confirmation.title == "Cancel audio separation?" &&
                   confirmation.buttons.size() == 2 &&
                   confirmation.buttons[0].id == kCancelTaskButton &&
                   confirmation.initiallySelectedButton ==
                       kKeepRunningButton,
               "cancellation must require a safe-default confirmation");

  task.cancellable = false;
  ok &= expect(layout(bounds, task).buttons.buttons.empty(),
               "a non-cancellable task must not expose a fake action");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
