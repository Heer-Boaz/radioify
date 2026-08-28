#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "app/media_processing_actions.h"
#include "app/media_processing_coordinator.h"
#include "core/native_wait_handle.h"
#include "media_task_presentation.h"

namespace tui_media_tasks {

struct Snapshot {
  std::optional<MediaTaskCardModel> activeCard;
  std::optional<MediaTaskStatusModel> latestStatus;
  std::optional<MediaTaskFailureDialogModel> latestFailure;
};

struct Update {
  bool changed = false;
  bool layoutChanged = false;
  std::vector<media_processing::TaskCompletion> completions;
};

// Shell-facing owner of background-task interaction and presentation. The
// application coordinator still owns the workers; this controller is the one
// place where the TUI polls them, handles cancellation and freezes their state
// into a renderable snapshot.
class Controller {
 public:
  Controller(media_processing::Coordinator& coordinator,
             media_processing::Actions& actions);

  std::optional<playback_media_processing::ActionResult> execute(
      const media_processing::ActionRequest& request);
  playback_media_actions::Context contextForSource(
      const std::filesystem::path& sourceFile) const;

  Update poll();
  bool cancelActive(media_processing::TaskId expectedTask);
  NativeWaitHandle waitHandle() const;

  const Snapshot& snapshot() const { return snapshot_; }

 private:
  void refreshSnapshot();
  static bool statusVisible(const Snapshot& snapshot);

  media_processing::Coordinator& coordinator_;
  media_processing::Actions& actions_;
  MediaTaskPresenter presenter_;
  Snapshot snapshot_;
};

}  // namespace tui_media_tasks
