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

#include "core/native_wait_handle.h"
#include "core/wake_event.h"
#include "playback/video/analysis/edit_review.h"

namespace playback_video_analysis {

struct AnalysisProgress;
struct ReviewJobResult {
  size_t visualSampleCount = 0;
  std::vector<EditProposal> suggestions;
};

enum class JobState : uint8_t {
  Idle,
  Running,
  Pausing,
  Succeeded,
  Failed,
  Cancelled,
};

struct JobRequest {
  std::filesystem::path sourcePath;
  int videoStreamIndex = -1;
  int64_t durationUs = 0;
  bool forceReanalysis = false;
  std::optional<TextEvidence> englishText;
};

struct JobSnapshot {
  JobState state = JobState::Idle;
  double progress = 0.0;
  std::string phase;
  std::string error;
  size_t visualSampleCount = 0;
  std::vector<EditProposal> suggestions;

  // A pause request is asynchronous: the operation still owns its resources
  // until it acknowledges the request with a terminal result.
  bool busy() const {
    return state == JobState::Running || state == JobState::Pausing;
  }
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
      ReviewJobResult*, std::string*)>;

  SceneAnalysisJob();
  explicit SceneAnalysisJob(WakeNotifier ownerWake);
  explicit SceneAnalysisJob(Operation operation);
  SceneAnalysisJob(Operation operation, WakeNotifier ownerWake);
  ~SceneAnalysisJob();

  SceneAnalysisJob(const SceneAnalysisJob&) = delete;
  SceneAnalysisJob& operator=(const SceneAnalysisJob&) = delete;

  bool start(JobRequest request);
  bool cancel();
  void requestStop();
  bool stopReady() const;
  bool finishStop();
  void stop();

  JobSnapshot snapshot() const;
  std::optional<JobSnapshot> takeCompletion();
  bool consumeChanged();
  NativeWaitHandle nativeWaitHandle() const;
  NativeWaitHandle workerWaitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_analysis
