#pragma once

#include <string>

#include "app/media_processing_coordinator.h"

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
