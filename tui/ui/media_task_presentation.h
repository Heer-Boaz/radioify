#pragma once

#include <optional>
#include <string>

namespace media_processing {
class Coordinator;
struct TaskActivity;
struct TaskCompletion;
}

struct MediaTaskActionHint {
  std::string shortcut;
  std::string label;
};

struct MediaTaskCardModel {
  std::string title;
  std::string sourceName;
  std::string detail;
  std::optional<MediaTaskActionHint> cancelAction;
  std::optional<float> progress;
};

struct MediaTaskStatusModel {
  std::string text;
  bool succeeded = false;
};

MediaTaskCardModel mediaTaskCardModel(
    const media_processing::TaskActivity& activity);
MediaTaskStatusModel mediaTaskStatusModel(
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

 private:
  const media_processing::Coordinator& coordinator_;
};
