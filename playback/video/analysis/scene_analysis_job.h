#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "playback/video/analysis/scene_analysis.h"

namespace playback_video_analysis {

enum class JobState : uint8_t {
  Idle,
  Running,
  Succeeded,
  Failed,
  Cancelled,
};

struct JobRequest {
  std::filesystem::path sourcePath;
  int videoStreamIndex = -1;
  int64_t durationUs = 0;
  bool forceReanalysis = false;
};

struct JobSnapshot {
  JobState state = JobState::Idle;
  uint64_t generation = 0;
  double progress = 0.0;
  std::string phase;
  std::string error;
  size_t visualSampleCount = 0;
  bool usedIndexedTranscript = false;
  std::vector<SceneSuggestion> suggestions;

  bool running() const { return state == JobState::Running; }
  bool finished() const {
    return state == JobState::Succeeded || state == JobState::Failed ||
           state == JobState::Cancelled;
  }
};

// Worker boundary owned by the video-edit workspace. The job publishes
// immutable snapshots; it never reaches into selection, document, player, or
// renderer state.
class SceneAnalysisJob {
 public:
  SceneAnalysisJob();
  ~SceneAnalysisJob();

  SceneAnalysisJob(const SceneAnalysisJob&) = delete;
  SceneAnalysisJob& operator=(const SceneAnalysisJob&) = delete;

  bool start(JobRequest request);
  void cancel();
  void stop();

  JobSnapshot snapshot() const;
  bool consumeChanged();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_analysis
