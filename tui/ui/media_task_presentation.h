#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "app/media_processing_task_id.h"
#include "dialog.h"
#include "playback/media_action_catalog.h"
#include "playback/media_processing_service.h"

namespace media_processing {
class Coordinator;
struct TaskActivity;
struct TaskCompletion;
}

struct MediaTaskCardModel {
  media_processing::TaskId taskId;
  playback_media_processing::Operation operation =
      playback_media_processing::Operation::MelodyAnalysis;
  std::string title;
  std::string operationName;
  std::string sourceName;
  std::string engineName;
  std::string detail;
  std::optional<float> progress;
  bool cancellable = false;
  bool cancelling = false;
};

enum class MediaTaskStatusTone {
  Neutral,
  Success,
  Error,
};

struct MediaTaskStatusModel {
  std::string text;
  MediaTaskStatusTone tone = MediaTaskStatusTone::Neutral;
};

inline constexpr tui_dialog::ButtonId kMediaTaskDialogClose = 0;
inline constexpr tui_dialog::ButtonId kMediaTaskDialogRetry = 1;

struct MediaTaskFailureDialogModel {
  media_processing::TaskId taskId;
  tui_dialog::Content content;
  std::filesystem::path sourceFile;
  std::optional<playback_media_actions::Action> retryAction;
};

MediaTaskCardModel mediaTaskCardModel(
    const media_processing::TaskActivity& activity);
MediaTaskStatusModel mediaTaskStatusModel(
    const media_processing::TaskCompletion& completion);
std::optional<MediaTaskFailureDialogModel> mediaTaskFailureDialogModel(
    const media_processing::TaskCompletion& completion);

// Read-only adapter from application task state to TUI view models. It owns no
// task lifecycle and keeps application services out of rendering code.
class MediaTaskPresenter {
 public:
  explicit MediaTaskPresenter(
      const media_processing::Coordinator& coordinator)
      : coordinator_(coordinator) {}

  std::optional<MediaTaskCardModel> activeCard() const;
  std::optional<MediaTaskStatusModel> latestStatus() const;
  std::optional<MediaTaskFailureDialogModel> latestFailure() const;

 private:
  const media_processing::Coordinator& coordinator_;
};
