#pragma once

#include <string>

namespace media_processing {
struct TaskActivity;
struct TaskCompletion;
}

struct MediaTaskCardModel {
  std::string title;
  std::string sourceName;
  std::string detail;
  float progress = 0.0f;
  bool cancellable = false;
};

struct MediaTaskStatusModel {
  std::string text;
  bool succeeded = false;
};

MediaTaskCardModel mediaTaskCardModel(
    const media_processing::TaskActivity& activity);
MediaTaskStatusModel mediaTaskStatusModel(
    const media_processing::TaskCompletion& completion);
