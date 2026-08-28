#include "media_task_controller.h"

#include <utility>

namespace tui_media_tasks {

Controller::Controller(media_processing::Coordinator& coordinator,
                       media_processing::Actions& actions)
    : coordinator_(coordinator),
      actions_(actions),
      presenter_(coordinator) {
  refreshSnapshot();
}

std::optional<playback_media_processing::ActionResult> Controller::execute(
    const media_processing::ActionRequest& request) {
  std::optional<playback_media_processing::ActionResult> result =
      actions_.execute(request);
  if (result) {
    refreshSnapshot();
  }
  return result;
}

playback_media_actions::Context Controller::contextForSource(
    const std::filesystem::path& sourceFile) const {
  return actions_.contextForSource(sourceFile);
}

Update Controller::poll() {
  const bool statusWasVisible = statusVisible(snapshot_);
  media_processing::PollResult processingUpdate = coordinator_.poll();
  refreshSnapshot();

  Update update;
  update.changed = processingUpdate.changed;
  update.layoutChanged =
      statusWasVisible != statusVisible(snapshot_);
  update.completions = std::move(processingUpdate.completions);
  return update;
}

bool Controller::cancelActive(media_processing::TaskId expectedTask) {
  if (!coordinator_.cancelActive(expectedTask)) {
    return false;
  }
  refreshSnapshot();
  return true;
}

NativeWaitHandle Controller::waitHandle() const {
  return coordinator_.waitHandle();
}

void Controller::refreshSnapshot() {
  snapshot_.activeCard = presenter_.activeCard();
  snapshot_.latestStatus = presenter_.latestStatus();
  snapshot_.latestFailure = presenter_.latestFailure();
}

bool Controller::statusVisible(const Snapshot& snapshot) {
  return snapshot.latestStatus && !snapshot.latestStatus->text.empty();
}

}  // namespace tui_media_tasks
