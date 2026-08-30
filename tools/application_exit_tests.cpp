#include <cstdlib>
#include <iostream>
#include <optional>
#include <variant>

#include "tui/ui/application_exit.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "application_exit_tests: " << message << '\n';
  return false;
}

playback_media_processing::Activity activeTask(
    playback_media_processing::TaskId id = playback_media_processing::TaskId{
        42}) {
  playback_media_processing::Activity task;
  task.taskId = id;
  task.operation = playback_media_processing::Operation::AudioSeparation;
  task.sourceFile = "NTE.mp4";
  task.progress = 0.42f;
  task.cancellable = true;
  return task;
}

bool quits(const tui_application_exit::Transition& transition) {
  return transition.intent &&
         std::holds_alternative<tui_application_exit::QuitNow>(
             *transition.intent);
}

const tui_application_exit::CancelTask* cancellation(
    const tui_application_exit::Transition& transition) {
  return transition.intent ? std::get_if<tui_application_exit::CancelTask>(
                                 &*transition.intent)
                           : nullptr;
}

}  // namespace

int main() {
  using namespace tui_application_exit;
  bool ok = true;

  Controller controller;
  Transition transition = controller.request(std::nullopt);
  ok &= expect(quits(transition) && !controller.pending(),
               "quit without background work must remain immediate");

  const playback_media_processing::Activity running = activeTask();
  transition = controller.request(running);
  ok &= expect(
      controller.pending() && transition.openDialog &&
          transition.openDialog->buttons.size() == 2 &&
          transition.openDialog->buttons.front().id ==
              kQuitAndCancelTaskButton &&
          transition.openDialog->buttons.back().id == kKeepRadioifyOpenButton &&
          transition.openDialog->initiallySelectedButton ==
              kKeepRadioifyOpenButton,
      "a running task must require an explicit, safe-default quit "
      "decision");
  controller.opened(tui_dialog::DialogId{10});
  transition = controller.handle(
      {tui_dialog::DialogId{10}, kKeepRadioifyOpenButton}, running);
  ok &=
      expect(transition.handled && !transition.intent && !controller.pending(),
             "the safe action must leave both the task and application "
             "running");

  transition = controller.request(running);
  controller.opened(tui_dialog::DialogId{11});
  transition = controller.handle(
      {tui_dialog::DialogId{11}, kQuitAndCancelTaskButton}, running);
  const CancelTask* cancel = cancellation(transition);
  ok &= expect(transition.handled && cancel &&
                   cancel->taskId == running.taskId && controller.pending(),
               "destructive confirmation must target the exact task and "
               "wait for its owner");
  controller.dismissed(tui_dialog::DialogId{11});

  playback_media_processing::Activity cancelling = running;
  cancelling.cancellable = false;
  cancelling.cancelling = true;
  transition = controller.resolveCancellation(running.taskId, true, cancelling);
  ok &= expect(transition.openDialog &&
                   transition.openDialog->buttons.size() == 1 &&
                   controller.pending(),
               "accepted cancellation must keep a visible asynchronous quit "
               "state");
  controller.opened(tui_dialog::DialogId{12});
  transition = controller.synchronize(cancelling);
  ok &= expect(!transition.intent && !transition.dismissDialog,
               "the application must stay alive while cancellation runs");
  transition = controller.synchronize(std::nullopt);
  ok &= expect(quits(transition) &&
                   transition.dismissDialog == tui_dialog::DialogId{12} &&
                   !controller.pending(),
               "quit may complete only after the represented task ends");

  playback_media_processing::Activity publishing = running;
  publishing.cancellable = false;
  transition = controller.request(publishing);
  ok &= expect(transition.openDialog &&
                   transition.openDialog->buttons.size() == 1 &&
                   controller.pending(),
               "publication past the cancellation barrier must be waited for, "
               "not interrupted");
  controller.opened(tui_dialog::DialogId{20});
  controller.dismissed(tui_dialog::DialogId{20});
  ok &= expect(
      !controller.pending() && !controller.synchronize(std::nullopt).intent,
      "Escape or Keep open must cancel only the pending quit");

  transition = controller.request(running);
  controller.opened(tui_dialog::DialogId{30});
  transition = controller.synchronize(publishing);
  ok &= expect(transition.dismissDialog == tui_dialog::DialogId{30} &&
                   transition.openDialog &&
                   transition.openDialog->buttons.size() == 1,
               "a task crossing its publication barrier must replace a stale "
               "cancellation prompt with an honest wait state");
  controller.opened(tui_dialog::DialogId{31});
  transition = controller.handle(
      {tui_dialog::DialogId{999}, kKeepRadioifyOpenButton}, publishing);
  ok &= expect(!transition.handled && controller.pending(),
               "an unrelated dialog activation must never resolve exit");
  controller.dismissed(tui_dialog::DialogId{31});

  transition = controller.request(running);
  controller.opened(tui_dialog::DialogId{40});
  transition = controller.handle(
      {tui_dialog::DialogId{40}, kQuitAndCancelTaskButton}, running);
  transition = controller.resolveCancellation(running.taskId, false, running);
  ok &= expect(transition.openDialog && !transition.openDialog->text.empty() &&
                   transition.openDialog->text.front().tone ==
                       tui_dialog::TextTone::Error &&
                   controller.pending(),
               "a rejected cancellation must remain visible and retryable");

  Controller completedWhileConfirming;
  transition = completedWhileConfirming.request(running);
  completedWhileConfirming.opened(tui_dialog::DialogId{50});
  transition = completedWhileConfirming.handle(
      {tui_dialog::DialogId{50}, kQuitAndCancelTaskButton}, std::nullopt);
  ok &= expect(transition.handled && quits(transition) &&
                   !completedWhileConfirming.pending(),
               "completion before destructive activation must quit without "
               "targeting a vanished task");

  const playback_media_processing::Activity replacementTask =
      activeTask(playback_media_processing::TaskId{84});
  Controller replacedWhileConfirming;
  transition = replacedWhileConfirming.request(running);
  replacedWhileConfirming.opened(tui_dialog::DialogId{60});
  transition = replacedWhileConfirming.handle(
      {tui_dialog::DialogId{60}, kQuitAndCancelTaskButton}, replacementTask);
  ok &= expect(transition.handled && !transition.intent &&
                   transition.openDialog &&
                   transition.openDialog->buttons.size() == 2 &&
                   replacedWhileConfirming.pending(),
               "destructive activation must replace a stale prompt when a "
               "new task owns the slot");

  Controller replacedWhileResolving;
  transition = replacedWhileResolving.request(running);
  replacedWhileResolving.opened(tui_dialog::DialogId{70});
  transition = replacedWhileResolving.handle(
      {tui_dialog::DialogId{70}, kQuitAndCancelTaskButton}, running);
  transition = replacedWhileResolving.resolveCancellation(running.taskId, false,
                                                          replacementTask);
  ok &= expect(!transition.intent && transition.openDialog &&
                   transition.openDialog->buttons.size() == 2 &&
                   replacedWhileResolving.pending(),
               "cancellation resolution must follow a replacement task "
               "instead of applying a stale result");

  Controller crossedBarrierWhileResolving;
  transition = crossedBarrierWhileResolving.request(running);
  crossedBarrierWhileResolving.opened(tui_dialog::DialogId{80});
  transition = crossedBarrierWhileResolving.handle(
      {tui_dialog::DialogId{80}, kQuitAndCancelTaskButton}, running);
  transition = crossedBarrierWhileResolving.resolveCancellation(
      running.taskId, false, publishing);
  ok &= expect(transition.openDialog &&
                   transition.openDialog->buttons.size() == 1 &&
                   crossedBarrierWhileResolving.pending(),
               "a rejected stale Cancel must immediately become a "
               "non-cancellable publication wait");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
