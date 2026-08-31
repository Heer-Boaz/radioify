#include <cstdlib>
#include <filesystem>
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
      (kind == MouseEventKind::Press || kind == MouseEventKind::Release)
          ? MouseButton::Left
          : MouseButton::None;
  event.mouse.buttons =
      leftButtonDown ? MouseButtons::Left : MouseButtons::None;
  return event;
}

InputEvent keyEvent(WORD key, char ch = 0, DWORD control = 0,
                    KeyPressKind pressKind = KeyPressKind::Initial,
                    std::uint32_t repeatCount = 1) {
  InputEvent event;
  event.type = InputEvent::Type::Key;
  event.key.vk = key;
  event.key.ch = ch;
  event.key.control = control;
  event.key.pressKind = pressKind;
  event.key.repeatCount = repeatCount;
  return event;
}

}  // namespace

int main() {
  using namespace tui_media_task_panel;
  bool ok = true;

  MediaTaskCardModel task;
  task.taskId = media_processing::TaskId{42};
  task.operation = playback_media_processing::Operation::AudioSeparation;
  task.title = "Separating audio";
  task.operationName = "audio separation";
  task.sourceName = "NTE.mp4";
  task.engineName = "NVIDIA TensorRT-RTX (native Windows)";
  task.detail = "Separating dialogue, music and effects on GPU";
  task.progress = 0.10f;
  task.cancellable = true;

  const Bounds bounds{100, 30, 2};
  const IndicatorLayout noIndicator;
  const Layout card = layout(bounds, task);
  ok &=
      expect(card.valid && card.width >= 46 && card.contentRows == 5 &&
                 card.buttons.buttons.size() == 2,
             "an active task must expose direct Cancel and Hide buttons");

  State state;
  state.synchronize(task);
  const tui_button_row::Placement& cancel = card.buttons.buttons.front();
  Interaction interaction =
      state.handle(pointerEvent(MouseEventKind::Move, cancel.x, card.buttons.y),
                   bounds, noIndicator, task);
  ok &= expect(
      interaction.consumed && interaction.changed && state.hoveredButton() == 0,
      "hovering the task button must be visible and consume the "
      "covered browser cell");

  State compactState;
  compactState.synchronize(task);
  const Bounds compactBounds{80, 6, 2};
  const Layout compactLayout = layout(compactBounds, task);
  ok &= expect(compactLayout.buttons.buttons.size() == 2 &&
                   compactLayout.buttons.rowCount == 1 &&
                   compactLayout.contentRows == 1,
               "a short surface must preserve the complete action row before "
               "optional task detail");
  interaction =
      compactState.handle(keyEvent(VK_TAB), compactBounds, noIndicator, task);
  ok &= expect(interaction.consumed && compactState.focused(),
               "responsive compact actions must remain keyboard reachable");

  const Bounds chromeConstrainedBounds{49, 4, 3};
  const Layout chromeConstrainedLayout = layout(chromeConstrainedBounds, task);
  ok &= expect(chromeConstrainedLayout.valid &&
                   chromeConstrainedLayout.y == 0 &&
                   chromeConstrainedLayout.buttons.buttons.size() == 2 &&
                   chromeConstrainedLayout.contentRows == 1,
               "active task actions must reclaim browser chrome on an "
               "extremely short terminal");

  State narrowState;
  narrowState.synchronize(task);
  const Bounds narrowBounds{12, 30, 2};
  const Layout narrowLayout = layout(narrowBounds, task);
  ok &= expect(narrowLayout.buttons.buttons.size() == 2 &&
                   narrowLayout.buttons.rowCount == 2 &&
                   narrowLayout.buttons.buttons[0].y !=
                       narrowLayout.buttons.buttons[1].y,
               "a narrow surface must stack complete labels instead of "
               "publishing clipped controls");
  interaction =
      narrowState.handle(keyEvent(VK_TAB), narrowBounds, noIndicator, task);
  ok &= expect(interaction.consumed && narrowState.focused(),
               "stacked task actions must remain focusable");
  interaction = narrowState.handle(keyEvent(VK_RIGHT), narrowBounds,
                                   noIndicator, task);
  ok &= expect(interaction.consumed &&
                   narrowState.highlightedButton() == 1,
               "arrow navigation must traverse vertically reflowed task "
               "actions");
  const tui_button_row::Placement& stackedHide =
      narrowLayout.buttons.buttons[1];
  interaction = narrowState.handle(
      pointerEvent(MouseEventKind::Press, stackedHide.x, stackedHide.y, true),
      narrowBounds, noIndicator, task);
  ok &= expect(interaction.consumed && !interaction.activatedAction,
               "a stacked action must arm on pointer press");
  interaction = narrowState.handle(
      pointerEvent(MouseEventKind::Release, stackedHide.x, stackedHide.y),
      narrowBounds, noIndicator, task);
  ok &= expect(interaction.consumed &&
                   interaction.activatedAction == Action::Hide &&
                   narrowState.hidden(),
               "a stacked action must activate on release at its own row");

  interaction = state.handle(
      pointerEvent(MouseEventKind::Press, cancel.x, card.buttons.y, true),
      bounds, noIndicator, task);
  ok &= expect(interaction.consumed && !interaction.activatedAction,
               "pressing Cancel must arm the button without activating it");
  interaction = state.handle(
      pointerEvent(MouseEventKind::Release, cancel.x, card.buttons.y), bounds,
      noIndicator, task);
  ok &= expect(
      interaction.consumed && interaction.activatedAction == Action::Cancel,
      "releasing over the armed Cancel button must publish its typed action");

  state.handle(
      pointerEvent(MouseEventKind::Press, cancel.x, card.buttons.y, true),
      bounds, noIndicator, task);
  interaction = state.handle(pointerEvent(MouseEventKind::Release, 0, 0),
                             bounds, noIndicator, task);
  ok &= expect(interaction.consumed && !interaction.activatedAction,
               "releasing away from an armed button must cancel the click "
               "without reaching browser content");

  interaction = state.handle(pointerEvent(MouseEventKind::Move, 0, 0), bounds,
                             noIndicator, task);
  ok &= expect(!interaction.consumed && !state.hoveredButton(),
               "leaving the panel must clear hover without blocking browser "
               "input");

  interaction = state.handle(keyEvent(VK_TAB), bounds, noIndicator, task);
  ok &= expect(interaction.consumed && interaction.focusChanged &&
                   state.focused() && state.highlightedButton() == 0,
               "Tab must move focus from the browser into an actionable "
               "task panel");
  interaction = state.handle(
      keyEvent(VK_RETURN, 0, 0, KeyPressKind::AutoRepeat, 4), bounds,
      noIndicator, task);
  ok &= expect(interaction.consumed && !interaction.activatedAction &&
                   state.focused(),
               "a repeated Enter must not activate a task action or leave "
               "the focused panel");
  interaction = state.handle(keyEvent(VK_SPACE, ' '), bounds, noIndicator,
                             task);
  ok &= expect(
      interaction.consumed && interaction.activatedAction == Action::Cancel,
      "Space must activate the selected task-panel button");
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

  interaction = state.handle(keyEvent(VK_TAB), bounds, noIndicator, task);
  ok &= expect(!interaction.consumed && !state.focused(),
               "a hidden indicator must not receive focus before it has "
               "visible geometry");

  const IndicatorLayout indicator = indicatorLayout(100, 25, task);
  ok &= expect(
      indicator.valid && indicator.actionVisible &&
          indicatorText(task) ==
              "Background task: NTE.mp4 - Separating audio 10%",
      "a hidden task must identify its source and expose an explicit Show "
      "action");
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

  interaction = state.handle(keyEvent(VK_TAB, 0, SHIFT_PRESSED), bounds,
                             noIndicator, task);
  ok &= expect(interaction.consumed && state.focused() &&
                   state.highlightedButton() == 1,
               "Shift+Tab must enter the panel in reverse focus order");
  interaction = state.handle(keyEvent(VK_TAB, 0, SHIFT_PRESSED), bounds,
                             noIndicator, task);
  ok &= expect(interaction.consumed && !state.focused(),
               "Shift+Tab must return reverse focus to the browser");

  state.handle(keyEvent(VK_TAB), bounds, noIndicator, task);
  interaction = state.handle(keyEvent('Q', 'q', LEFT_CTRL_PRESSED), bounds,
                             noIndicator, task);
  ok &= expect(!interaction.consumed && interaction.focusChanged &&
                   !state.focused(),
               "an unrelated accelerator must leave the modeless panel and "
               "continue through the shell router");

  state.handle(keyEvent(VK_TAB, 0, SHIFT_PRESSED), bounds, noIndicator, task);
  state.handle(keyEvent(VK_RETURN), bounds, noIndicator, task);
  interaction = state.handle(
      pointerEvent(MouseEventKind::Move, indicator.x, indicator.y), bounds,
      indicator, task);
  ok &= expect(!interaction.consumed && !state.indicatorHighlighted(),
               "read-only footer status must not masquerade as an action");
  interaction =
      state.handle(pointerEvent(MouseEventKind::Move, indicator.showX,
                                indicator.y),
                   bounds, indicator, task);
  ok &= expect(interaction.consumed && interaction.changed &&
                   state.indicatorHighlighted(),
               "the hidden-task indicator must expose pointer hover");
  interaction = state.handle(
      pointerEvent(MouseEventKind::Press, indicator.showX, indicator.y, true),
      bounds, indicator, task);
  ok &= expect(
      interaction.consumed && !interaction.activatedAction && state.hidden(),
      "pressing the footer indicator must arm it without restoring "
      "the panel early");
  interaction = state.handle(
      pointerEvent(MouseEventKind::Release, indicator.showX, indicator.y),
      bounds, indicator, task);
  ok &= expect(interaction.consumed && interaction.layoutChanged &&
                   interaction.activatedAction == Action::Show &&
                   !state.hidden() && !state.focused(),
               "clicking the footer indicator must restore the panel without "
               "stealing browser focus");

  const IndicatorLayout statusOnlyIndicator = indicatorLayout(7, 25, task);
  State statusOnlyState;
  statusOnlyState.synchronize(task);
  statusOnlyState.handle(keyEvent(VK_TAB), bounds, noIndicator, task);
  statusOnlyState.handle(keyEvent(VK_RIGHT), bounds, noIndicator, task);
  statusOnlyState.handle(keyEvent(VK_RETURN), bounds, noIndicator, task);
  interaction = statusOnlyState.handle(keyEvent(VK_TAB), bounds,
                                       statusOnlyIndicator, task);
  ok &= expect(statusOnlyIndicator.valid &&
                   !statusOnlyIndicator.actionVisible &&
                   !interaction.consumed && !statusOnlyState.focused(),
               "a clipped Show label must remain read-only and unfocusable");

  state.handle(keyEvent(VK_TAB), bounds, noIndicator, task);
  interaction = state.handle(
      pointerEvent(MouseEventKind::Press, 0, bounds.top + 10, true), bounds,
      noIndicator, task);
  ok &= expect(
      !interaction.consumed && interaction.focusChanged && !state.focused(),
      "clicking outside a focused panel must return focus and let "
      "the browser receive the click");

  const DialogRequest confirmation = cancellationDialogRequest(task);
  ok &= expect(
      confirmation.content.buttons.size() == 2 &&
          confirmation.content.buttons[0].id == kCancelTaskButton &&
          confirmation.content.initiallySelectedButton == kKeepRunningButton &&
          confirmation.context.taskId == task.taskId,
      "cancellation must require a safe-default confirmation");

  DialogSession dialogSession;
  const tui_dialog::DialogId cancellationDialogId{7};
  dialogSession.opened(cancellationDialogId, confirmation.context);
  ok &= expect(!dialogSession.synchronize(task, std::nullopt, {}),
               "a cancellation dialog must remain open for its exact active "
               "task");
  const std::optional<DialogIntent> cancellationIntent =
      dialogSession.handle({cancellationDialogId, kCancelTaskButton});
  const CancelTask* cancelTask =
      cancellationIntent ? std::get_if<CancelTask>(&*cancellationIntent)
                         : nullptr;
  ok &= expect(cancelTask && cancelTask->taskId == task.taskId,
               "a cancellation button must publish its bound task identity");

  const std::filesystem::path setupSource = LR"(C:\Media\setup.mp4)";
  const DialogRequest setupConfirmation =
      audioSeparationSetupDialogRequest(
          playback_media_processing::AudioSeparationSetupRequest{
              setupSource});
  const tui_dialog::DialogId setupDialogId{9};
  dialogSession.opened(setupDialogId, setupConfirmation.context);
  ok &= expect(
      !dialogSession.synchronize(
          std::nullopt, std::nullopt,
          [&](const std::filesystem::path& source) {
            return source == setupSource;
          }),
      "a setup dialog must remain open while its exact source still requires "
      "setup");
  ok &= expect(
      dialogSession.synchronize(
          std::nullopt, std::nullopt,
          [](const std::filesystem::path&) { return false; }) == setupDialogId,
      "a setup dialog must close as soon as current availability no longer "
      "requires setup");
  dialogSession.opened(setupDialogId, setupConfirmation.context);
  const auto declinedSetup =
      dialogSession.handle({setupDialogId, kNotNowButton});
  dialogSession.opened(setupDialogId, setupConfirmation.context);
  const auto confirmedSetup =
      dialogSession.handle({setupDialogId, kSetUpAudioButton});
  const SetUpAudioSeparation* setupIntent =
      confirmedSetup
          ? std::get_if<SetUpAudioSeparation>(&*confirmedSetup)
          : nullptr;
  ok &= expect(
      setupConfirmation.content.initiallySelectedButton == kNotNowButton &&
          !declinedSetup && setupIntent &&
          setupIntent->request.sourceFile == setupSource,
      "the setup dialog must default to declining and publish its typed "
      "request only after explicit confirmation");

  dialogSession.opened(cancellationDialogId, confirmation.context);
  MediaTaskCardModel differentTask = task;
  differentTask.taskId = media_processing::TaskId{99};
  ok &= expect(dialogSession.synchronize(differentTask, std::nullopt, {}) ==
                   cancellationDialogId,
               "a stale cancellation dialog must request only its own "
               "conditional dismissal");

  MediaTaskFailureDialogModel failure;
  failure.taskId = media_processing::TaskId{100};
  failure.sourceFile = "NTE.mp4";
  failure.retryAction = playback_media_actions::Action::SeparateAudio;
  failure.content.buttons = {{kMediaTaskDialogClose, "Close"},
                             {kMediaTaskDialogRetry, "Retry"}};
  const DialogRequest retryRequest = failureDialogRequest(failure);
  const tui_dialog::DialogId failureDialogId{8};
  dialogSession.opened(failureDialogId, retryRequest.context);
  ok &= expect(!dialogSession.synchronize(std::nullopt, failure, {}),
               "a failure dialog must remain current only while its exact "
               "failure is the latest result");
  const std::optional<DialogIntent> retryIntent =
      dialogSession.handle({failureDialogId, kMediaTaskDialogRetry});
  const RetryTask* retryTask =
      retryIntent ? std::get_if<RetryTask>(&*retryIntent) : nullptr;
  ok &= expect(retryTask && retryTask->taskId == failure.taskId &&
                   retryTask->sourceFile == failure.sourceFile &&
                   retryTask->action == *failure.retryAction,
               "a failure dialog must publish a typed retry for the failure "
               "it displays");

  dialogSession.opened(failureDialogId, retryRequest.context);
  MediaTaskFailureDialogModel newerFailure = failure;
  newerFailure.taskId = media_processing::TaskId{101};
  ok &= expect(dialogSession.synchronize(std::nullopt, newerFailure, {}) ==
                   failureDialogId,
               "a newer completion must invalidate an open stale failure "
               "dialog");

  DeferredFailureState deferredFailure;
  deferredFailure.observe(failure, std::nullopt);
  ok &= expect(deferredFailure.pending(),
               "a failure may be deferred while playback owns the terminal");
  MediaTaskCardModel newerTask = task;
  newerTask.taskId = media_processing::TaskId{102};
  deferredFailure.synchronize(newerTask);
  ok &= expect(!deferredFailure.pending(),
               "starting newer work must retire a deferred old failure");
  deferredFailure.observe(failure, newerTask);
  ok &= expect(!deferredFailure.pending(),
               "an old completion must not be deferred behind newer active "
               "work");
  deferredFailure.observe(failure, std::nullopt);
  const std::optional<MediaTaskFailureDialogModel> presentedFailure =
      deferredFailure.take();
  ok &= expect(presentedFailure && presentedFailure->taskId == failure.taskId &&
                   !deferredFailure.pending() && !deferredFailure.take(),
               "a deferred failure must be presented at most once");

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

  MediaTaskCardModel replacement = task;
  replacement.taskId = media_processing::TaskId{43};
  state.synchronize(replacement);
  state.handle(keyEvent(VK_TAB), bounds, noIndicator, replacement);
  state.handle(keyEvent(VK_RETURN), bounds, noIndicator, replacement);
  ok &= expect(state.hidden(), "the replacement task must remain actionable");
  replacement.taskId = media_processing::TaskId{44};
  state.synchronize(replacement);
  ok &= expect(!state.hidden() && !state.focused(),
               "a new task identity must not inherit hidden or focused state "
               "from its predecessor");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
