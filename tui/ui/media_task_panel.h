#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "tui/input_event.h"
#include "tui/ui/button_row.h"
#include "tui/ui/media_task_presentation.h"
#include "playback/media_processing_actions.h"

namespace tui_media_task_panel {

enum class Action : tui_button_row::ButtonId {
  Cancel = 1,
  Hide = 2,
  Show = 3,
};

enum class ActivationSource {
  Keyboard,
  Pointer,
};

inline constexpr tui_dialog::ButtonId kCancelTaskButton = 2;
inline constexpr tui_dialog::ButtonId kKeepRunningButton = 3;
inline constexpr tui_dialog::ButtonId kSetUpAudioButton = 4;
inline constexpr tui_dialog::ButtonId kNotNowButton = 5;

struct Bounds {
  int width = 0;
  int height = 0;
  int top = 0;
};

struct Layout {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int innerWidth = 0;
  int progressY = 0;
  int contentRows = 0;
  tui_button_row::Layout buttons;
  bool valid = false;

  bool contains(int pointerX, int pointerY) const;
};

struct IndicatorLayout {
  int x = 0;
  int y = -1;
  int width = 0;
  int statusWidth = 0;
  int showX = 0;
  int showWidth = 0;
  std::string statusText;
  bool valid = false;
  bool actionVisible = false;

  bool contains(int pointerX, int pointerY) const;
};

struct Interaction {
  bool consumed = false;
  bool changed = false;
  bool layoutChanged = false;
  bool focusChanged = false;
  std::optional<Action> activatedAction;
};

struct CancelTask {
  media_processing::TaskId taskId;
};

struct RetryTask {
  media_processing::TaskId taskId;
  std::filesystem::path sourceFile;
  playback_media_actions::Action action;
};

struct SetUpAudioSeparation {
  playback_media_processing::AudioSeparationSetupRequest request;
};

using DialogIntent =
    std::variant<CancelTask, RetryTask, SetUpAudioSeparation>;

enum class DialogKind {
  Cancellation,
  Failure,
  AudioSeparationSetup,
};

struct DialogContext {
  DialogKind kind = DialogKind::Cancellation;
  media_processing::TaskId taskId;
  std::filesystem::path sourceFile;
  std::optional<playback_media_actions::Action> retryAction;
};

struct DialogRequest {
  tui_dialog::Content content;
  DialogContext context;
};

using AudioSeparationSetupRequired =
    std::function<bool(const std::filesystem::path&)>;

// Correlates generic dialog sessions with media-task intent. Button IDs never
// escape into the shell composition root, and a cancellation prompt is
// invalidated as soon as its exact task is no longer cancellable.
class DialogSession {
 public:
  void opened(tui_dialog::DialogId dialog, DialogContext context);
  std::optional<tui_dialog::DialogId> synchronize(
      const std::optional<MediaTaskCardModel>& activeTask,
      const std::optional<MediaTaskFailureDialogModel>& latestFailure,
      const AudioSeparationSetupRequired& setupRequired);
  std::optional<DialogIntent> handle(
      const tui_dialog::ButtonActivation& activation);
  void dismissed(tui_dialog::DialogId dialog);

 private:
  std::optional<tui_dialog::DialogId> dialog_;
  std::optional<DialogContext> context_;
};

// Owns automatic error presentation while playback temporarily prevents the
// browser shell from showing a dialog. A new task or any newer completion
// supersedes the deferred failure; errors never reappear outside the lifecycle
// that produced them.
class DeferredFailureState {
 public:
  void observe(
      std::optional<MediaTaskFailureDialogModel> completedTaskFailure,
      const std::optional<MediaTaskCardModel>& activeTask);
  void synchronize(const std::optional<MediaTaskCardModel>& activeTask);
  std::optional<MediaTaskFailureDialogModel> take();

  bool pending() const { return pending_.has_value(); }

 private:
  std::optional<MediaTaskFailureDialogModel> pending_;
};

class State {
 public:
  void synchronize(const std::optional<MediaTaskCardModel>& task);
  Interaction handle(const InputEvent& event, const Bounds& bounds,
                     const IndicatorLayout& indicator,
                     const MediaTaskCardModel& task);

  bool focused() const { return focused_; }
  bool hidden() const { return hidden_; }
  bool visible(const std::optional<MediaTaskCardModel>& task) const {
    return task.has_value() && !hidden_;
  }
  bool indicatorVisible(const std::optional<MediaTaskCardModel>& task) const {
    return task.has_value() && hidden_;
  }
  bool show();
  std::optional<std::size_t> highlightedButton() const;
  bool indicatorHighlighted() const {
    return hidden_ && (focused_ || buttonPointer_.hovered().has_value());
  }
  std::optional<std::size_t> hoveredButton() const {
    return hidden_ ? std::nullopt : buttonPointer_.hovered();
  }

 private:
  void setFocused(bool focused, Interaction& interaction);
  void activate(Action action, ActivationSource source,
                Interaction& interaction);

  bool hidden_ = false;
  bool focused_ = false;
  std::size_t selectedButton_ = 0;
  tui_button_row::PointerState buttonPointer_;
  std::optional<media_processing::TaskId> taskId_;
  std::vector<tui_button_row::ButtonId> actionIds_;
};

std::vector<tui_button_row::Button> actionsFor(const MediaTaskCardModel& task);
Layout layout(const Bounds& bounds, const MediaTaskCardModel& task);
std::string indicatorText(const MediaTaskCardModel& task);
IndicatorLayout indicatorLayout(int availableWidth, int y,
                                const MediaTaskCardModel& task);
DialogRequest cancellationDialogRequest(const MediaTaskCardModel& task);
DialogRequest audioSeparationSetupDialogRequest(
    playback_media_processing::AudioSeparationSetupRequest request);
DialogRequest failureDialogRequest(const MediaTaskFailureDialogModel& failure);

}  // namespace tui_media_task_panel
