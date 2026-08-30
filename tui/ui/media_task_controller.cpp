#include "media_task_controller.h"

#include <utility>

#include "core/path_identity.h"

namespace tui_media_tasks {

Controller::Controller(media_processing::Coordinator& coordinator,
                       media_processing::Actions& actions)
    : coordinator_(coordinator), actions_(actions) {
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

std::optional<playback_media_processing::ActionResult> Controller::retry(
    media_processing::TaskId expectedFailure,
    const media_processing::ActionRequest& request) {
  refreshSnapshot();
  if (snapshot_.activeActivity || !snapshot_.latestCompletion ||
      snapshot_.latestCompletion->id != expectedFailure ||
      snapshot_.latestCompletion->outcome !=
          media_processing::TaskOutcome::Failed) {
    return std::nullopt;
  }
  return execute(request);
}

std::optional<MediaTaskCardModel> Controller::cancellationTarget(
    const playback_media_processing::CancellationRequest& request) {
  refreshSnapshot();
  const std::optional<playback_media_processing::Activity>& activity =
      snapshot_.activeActivity;
  if (!request.taskId || !activity || activity->taskId != request.taskId ||
      !activity->cancellable ||
      !samePath(activity->sourceFile, request.sourceFile)) {
    return std::nullopt;
  }

  if (request.operation != activity->operation) {
    return std::nullopt;
  }

  return snapshot_.activeCard &&
                 snapshot_.activeCard->taskId == activity->taskId
             ? snapshot_.activeCard
             : std::nullopt;
}

bool Controller::confirmCancellation(
    const playback_media_processing::CancellationRequest& request) {
  const std::optional<MediaTaskCardModel> target = cancellationTarget(request);
  if (!target || target->taskId != request.taskId) return false;
  const playback_media_processing::ActionResult result =
      actions_.playbackActions().confirmCancellation(request);
  refreshSnapshot();
  return result.accepted;
}

playback_media_processing::ActionResult Controller::confirmAudioSeparationSetup(
    const playback_media_processing::AudioSeparationSetupRequest& request) {
  playback_media_processing::ActionResult result =
      actions_.playbackActions().confirmAudioSeparationSetup(request);
  refreshSnapshot();
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
  update.layoutChanged = statusWasVisible != statusVisible(snapshot_);
  update.completions = std::move(processingUpdate.completions);
  return update;
}

bool Controller::cancelActive(media_processing::TaskId expectedTask) {
  const bool accepted = coordinator_.cancelActive(expectedTask);
  // Cancellation races the worker's commit barrier. Refresh on rejection too,
  // so callers can distinguish a stale Cancel surface from a real backend
  // failure without waiting for the next event-loop poll.
  refreshSnapshot();
  return accepted;
}

NativeWaitHandle Controller::waitHandle() const {
  return coordinator_.waitHandle();
}

void Controller::refreshSnapshot() {
  const std::optional<media_processing::TaskActivity> activity =
      coordinator_.activity();
  snapshot_.activeActivity = media_processing::activityForPlayback(activity);
  snapshot_.activeCard =
      activity
          ? std::optional<MediaTaskCardModel>(mediaTaskCardModel(*activity))
          : std::nullopt;

  snapshot_.latestCompletion = coordinator_.latestCompletion();
  snapshot_.latestStatus =
      snapshot_.latestCompletion
          ? std::optional<MediaTaskStatusModel>(
                mediaTaskStatusModel(*snapshot_.latestCompletion))
          : std::nullopt;
  snapshot_.latestFailure =
      snapshot_.latestCompletion
          ? mediaTaskFailureDialogModel(*snapshot_.latestCompletion)
          : std::nullopt;
}

bool Controller::statusVisible(const Snapshot& snapshot) {
  return snapshot.latestStatus && !snapshot.latestStatus->text.empty();
}

}  // namespace tui_media_tasks
