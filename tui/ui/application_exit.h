#pragma once

#include <optional>
#include <variant>

#include "tui/ui/media_task_presentation.h"

namespace tui_application_exit {

inline constexpr tui_dialog::ButtonId kQuitAndCancelTaskButton = 1;
inline constexpr tui_dialog::ButtonId kKeepRadioifyOpenButton = 2;

struct QuitNow {};

struct CancelTask {
  media_processing::TaskId taskId;
};

using Intent = std::variant<QuitNow, CancelTask>;

struct Transition {
  // True only when handle() recognized the supplied dialog activation.
  bool handled = false;
  std::optional<tui_dialog::Content> openDialog;
  std::optional<tui_dialog::DialogId> dismissDialog;
  std::optional<Intent> intent;
};

// Owns cooperative application-exit decisions while an offline media task is
// active. The shell remains alive while cancellation or final publication
// completes; this controller publishes intent but never blocks or joins a
// worker. Forced process termination (for example CTRL_CLOSE_EVENT from a
// closing console host) cannot offer this interaction contract; resilience to
// that boundary is a separate file-output crash-recovery responsibility.
class Controller {
 public:
  Transition request(
      const std::optional<MediaTaskCardModel>& activeTask);
  Transition synchronize(
      const std::optional<MediaTaskCardModel>& activeTask);
  Transition handle(
      const tui_dialog::ButtonActivation& activation,
      const std::optional<MediaTaskCardModel>& activeTask);
  Transition resolveCancellation(
      media_processing::TaskId taskId, bool accepted,
      const std::optional<MediaTaskCardModel>& activeTask);

  void opened(tui_dialog::DialogId dialog);
  void dismissed(tui_dialog::DialogId dialog);
  bool pending() const;

 private:
  enum class Phase {
    Idle,
    ConfirmingCancellation,
    AwaitingCancellationResult,
    WaitingForTask,
  };

  Transition beginFor(
      const std::optional<MediaTaskCardModel>& activeTask);
  Transition replaceFor(
      const std::optional<MediaTaskCardModel>& activeTask);
  void reset();

  Phase phase_ = Phase::Idle;
  std::optional<MediaTaskCardModel> task_;
  std::optional<tui_dialog::DialogId> dialog_;
};

}  // namespace tui_application_exit
