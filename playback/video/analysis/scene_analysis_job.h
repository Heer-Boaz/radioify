#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "playback/video/analysis/scene_analysis.h"

namespace playback_video_analysis {

struct AnalysisProgress;
struct AnalysisResult;

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

// Worker boundary owned by the video-edit workspace. Progress is published as
// immutable snapshots and each terminal result is consumed exactly once. The
// job never reaches into selection, document, player, or renderer state.
class SceneAnalysisJob {
 public:
  using ProgressReporter = std::function<void(const AnalysisProgress&)>;
  using Operation = std::function<bool(
      const JobRequest&, const ProgressReporter&, const std::atomic<bool>*,
      AnalysisResult*, std::string*)>;

  SceneAnalysisJob();
  explicit SceneAnalysisJob(Operation operation);
  ~SceneAnalysisJob();

  SceneAnalysisJob(const SceneAnalysisJob&) = delete;
  SceneAnalysisJob& operator=(const SceneAnalysisJob&) = delete;

  bool start(JobRequest request);
  bool cancel();
  void stop();

  JobSnapshot snapshot() const;
  std::optional<JobSnapshot> takeCompletion();
  bool consumeChanged();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_analysis
