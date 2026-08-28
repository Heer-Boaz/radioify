#include "tui/ui/application_exit.h"

#include <string>
#include <utility>

namespace tui_application_exit {
namespace {

tui_dialog::Content cancellationConfirmation(
    const MediaTaskCardModel& task, bool cancellationWasRejected = false) {
  tui_dialog::Content content;
  content.title = "Quit Radioify?";
  if (cancellationWasRejected) {
    content.text.push_back(
        {"The task could not be cancelled yet.",
         tui_dialog::TextTone::Error});
  }
  content.text.push_back(
      {"Background work is still running for " + task.sourceName + ".",
       tui_dialog::TextTone::Normal});
  content.text.push_back(
      {"Quitting will cancel " + task.operationName +
           " and discard its progress.",
       tui_dialog::TextTone::Normal});
  content.buttons.push_back(
      {kQuitAndCancelTaskButton, "Quit and cancel task", "Quit + cancel"});
  content.buttons.push_back(
      {kKeepRadioifyOpenButton, "Keep Radioify open", "Keep open"});
  content.initiallySelectedButton = kKeepRadioifyOpenButton;
  return content;
}

tui_dialog::Content waitingForTask(const MediaTaskCardModel& task) {
  tui_dialog::Content content;
  if (task.cancelling) {
    content.title = "Cancelling before quit";
    content.text.push_back(
        {"Radioify is cancelling " + task.operationName + " for " +
             task.sourceName + ".",
         tui_dialog::TextTone::Normal});
    content.text.push_back(
        {"Radioify will close when the task has stopped.",
         tui_dialog::TextTone::Secondary});
  } else {
    content.title = "Finishing before quit";
    content.text.push_back(
        {"Radioify is publishing the output for " + task.sourceName + ".",
         tui_dialog::TextTone::Normal});
    content.text.push_back(
        {"This final step can no longer be cancelled.",
         tui_dialog::TextTone::Secondary});
    content.text.push_back(
        {"Radioify will close when the output is safe.",
         tui_dialog::TextTone::Secondary});
  }
  content.buttons.push_back(
      {kKeepRadioifyOpenButton, "Keep Radioify open", "Keep open"});
  content.initiallySelectedButton = kKeepRadioifyOpenButton;
  return content;
}

Transition quitNow(std::optional<tui_dialog::DialogId> dismiss = {}) {
  Transition transition;
  transition.dismissDialog = dismiss;
  transition.intent = Intent{QuitNow{}};
  return transition;
}

}  // namespace

Transition Controller::request(
    const std::optional<MediaTaskCardModel>& activeTask) {
  if (phase_ != Phase::Idle) return {};
  return beginFor(activeTask);
}

Transition Controller::beginFor(
    const std::optional<MediaTaskCardModel>& activeTask) {
  if (!activeTask) {
    reset();
    return quitNow();
  }

  task_ = *activeTask;
  Transition transition;
  if (activeTask->cancellable) {
    phase_ = Phase::ConfirmingCancellation;
    transition.openDialog = cancellationConfirmation(*activeTask);
  } else {
    phase_ = Phase::WaitingForTask;
    transition.openDialog = waitingForTask(*activeTask);
  }
  return transition;
}

Transition Controller::replaceFor(
    const std::optional<MediaTaskCardModel>& activeTask) {
  const std::optional<tui_dialog::DialogId> obsolete = dialog_;
  reset();
  Transition transition = beginFor(activeTask);
  transition.dismissDialog = obsolete;
  return transition;
}

Transition Controller::synchronize(
    const std::optional<MediaTaskCardModel>& activeTask) {
  switch (phase_) {
    case Phase::Idle:
      return {};
    case Phase::ConfirmingCancellation:
      if (activeTask && task_ && activeTask->taskId == task_->taskId &&
          activeTask->cancellable) {
        return {};
      }
      return replaceFor(activeTask);
    case Phase::AwaitingCancellationResult:
      if (activeTask && task_ && activeTask->taskId == task_->taskId) {
        return {};
      }
      return replaceFor(activeTask);
    case Phase::WaitingForTask:
      if (activeTask && task_ && activeTask->taskId == task_->taskId) {
        task_ = *activeTask;
        return {};
      }
      return replaceFor(activeTask);
  }
  return {};
}

Transition Controller::handle(
    const tui_dialog::ButtonActivation& activation,
    const std::optional<MediaTaskCardModel>& activeTask) {
  Transition transition;
  if (!dialog_ || activation.dialog != *dialog_) return transition;
  transition.handled = true;
  dialog_.reset();

  if (phase_ == Phase::WaitingForTask ||
      activation.button != kQuitAndCancelTaskButton) {
    reset();
    return transition;
  }
  if (phase_ != Phase::ConfirmingCancellation || !task_) {
    reset();
    return transition;
  }
  if (!activeTask) {
    reset();
    transition.intent = Intent{QuitNow{}};
    return transition;
  }
  if (activeTask->taskId != task_->taskId) {
    reset();
    Transition replacement = beginFor(activeTask);
    replacement.handled = true;
    return replacement;
  }
  if (!activeTask->cancellable) {
    phase_ = Phase::WaitingForTask;
    task_ = *activeTask;
    transition.openDialog = waitingForTask(*activeTask);
    return transition;
  }

  phase_ = Phase::AwaitingCancellationResult;
  task_ = *activeTask;
  transition.intent = Intent{CancelTask{activeTask->taskId}};
  return transition;
}

Transition Controller::resolveCancellation(
    media_processing::TaskId taskId, bool accepted,
    const std::optional<MediaTaskCardModel>& activeTask) {
  if (phase_ != Phase::AwaitingCancellationResult || !task_ ||
      task_->taskId != taskId) {
    return {};
  }
  if (!activeTask) {
    reset();
    return quitNow();
  }
  if (activeTask->taskId != taskId) {
    reset();
    return beginFor(activeTask);
  }

  task_ = *activeTask;
  if (accepted || !activeTask->cancellable) {
    phase_ = Phase::WaitingForTask;
    task_->cancellable = false;
    task_->cancelling = accepted || activeTask->cancelling;
    Transition transition;
    transition.openDialog = waitingForTask(*task_);
    return transition;
  }

  phase_ = Phase::ConfirmingCancellation;
  Transition transition;
  transition.openDialog = cancellationConfirmation(*task_, true);
  return transition;
}

void Controller::opened(tui_dialog::DialogId dialog) {
  if (phase_ != Phase::Idle) dialog_ = dialog;
}

void Controller::dismissed(tui_dialog::DialogId dialog) {
  if (dialog_ && *dialog_ == dialog) reset();
}

bool Controller::pending() const { return phase_ != Phase::Idle; }

void Controller::reset() {
  phase_ = Phase::Idle;
  task_.reset();
  dialog_.reset();
}

}  // namespace tui_application_exit
