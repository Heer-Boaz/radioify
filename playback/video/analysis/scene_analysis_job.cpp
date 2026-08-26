#include "playback/video/analysis/scene_analysis_job.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include "playback/video/analysis/scene_analyzer.h"

namespace playback_video_analysis {

struct SceneAnalysisJob::Impl {
  explicit Impl(Operation analysisOperation)
      : operation(std::move(analysisOperation)) {}

  mutable std::mutex mutex;
  std::atomic<bool> changed{false};
  std::thread worker;
  std::atomic<bool> cancelled{false};
  Operation operation;
  JobSnapshot state;
  std::optional<JobSnapshot> completion;
  std::chrono::steady_clock::time_point lastProgressNotification =
      std::chrono::steady_clock::time_point::min();

  void updateProgress(double progress, const std::string& phase) {
    const auto now = std::chrono::steady_clock::now();
    bool notify = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (state.state != JobState::Running) return;
      progress = std::clamp(progress, 0.0, 1.0);
      if (phase != state.phase || progress >= 1.0 ||
          progress - state.progress >= 0.01 ||
          lastProgressNotification ==
              std::chrono::steady_clock::time_point::min() ||
          now - lastProgressNotification >= std::chrono::milliseconds(250)) {
        state.progress = progress;
        state.phase = phase;
        lastProgressNotification = now;
        notify = true;
      }
    }
    if (notify) changed.store(true, std::memory_order_release);
  }

  void run(JobRequest request) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    AnalysisResult result;
    std::string error;
    bool succeeded = false;
    try {
      succeeded = operation(
          request,
          [this](const AnalysisProgress& progress) {
            updateProgress(progress.fraction, progress.phase);
          },
          &cancelled, &result, &error);
    } catch (const std::exception& exception) {
      error = std::string("Segment detection failed: ") + exception.what();
    } catch (...) {
      error = "Segment detection failed unexpectedly.";
    }
    if (uninitializeCom) CoUninitialize();

    {
      std::lock_guard<std::mutex> lock(mutex);
      if (succeeded) {
        state.state = JobState::Succeeded;
        state.progress = 1.0;
        state.phase = "Segment detection complete";
        state.visualSampleCount = result.visualSampleCount;
        state.usedIndexedTranscript = !result.transcriptPath.empty();
        state.suggestions = std::move(result.suggestions);
      } else if (cancelled.load(std::memory_order_relaxed)) {
        state.state = JobState::Cancelled;
        state.phase = "Segment detection cancelled";
        state.error.clear();
      } else {
        state.state = JobState::Failed;
        state.phase = "Segment detection failed";
        state.error = error.empty() ? "Segment detection failed unexpectedly."
                                    : std::move(error);
      }
      completion = state;
    }
    changed.store(true, std::memory_order_release);
  }
};

SceneAnalysisJob::SceneAnalysisJob(Operation operation)
    : impl_(std::make_unique<Impl>(std::move(operation))) {}

SceneAnalysisJob::~SceneAnalysisJob() { stop(); }

bool SceneAnalysisJob::start(JobRequest request) {
  if (!impl_ || request.sourcePath.empty() || request.durationUs <= 0) {
    return false;
  }
  std::thread previous;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->operation || impl_->state.state == JobState::Running ||
        impl_->completion) {
      return false;
    }
    if (impl_->worker.joinable()) previous = std::move(impl_->worker);
  }
  if (previous.joinable()) previous.join();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cancelled.store(false, std::memory_order_relaxed);
    impl_->state = JobSnapshot{};
    impl_->state.state = JobState::Running;
    impl_->state.phase = "Starting segment detection";
    impl_->lastProgressNotification =
        std::chrono::steady_clock::time_point::min();
    try {
      impl_->worker = std::thread(
          [implementation = impl_.get(), request = std::move(request)]() mutable {
            implementation->run(std::move(request));
          });
    } catch (...) {
      impl_->state.state = JobState::Failed;
      impl_->state.phase = "Segment detection failed";
      impl_->state.error = "Could not start the segment-detection worker.";
      impl_->completion = impl_->state;
      impl_->changed.store(true, std::memory_order_release);
      return false;
    }
  }
  impl_->changed.store(true, std::memory_order_release);
  return true;
}

bool SceneAnalysisJob::cancel() {
  if (!impl_) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->state.state != JobState::Running ||
      impl_->cancelled.exchange(true, std::memory_order_relaxed)) {
    return false;
  }
  impl_->changed.store(true, std::memory_order_release);
  return true;
}

void SceneAnalysisJob::stop() {
  if (!impl_) return;
  impl_->cancelled.store(true, std::memory_order_relaxed);
  if (impl_->worker.joinable()) impl_->worker.join();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state.state == JobState::Running) {
      impl_->state.state = JobState::Cancelled;
      impl_->state.phase = "Segment detection cancelled";
    }
  }
  impl_->changed.store(false, std::memory_order_release);
}

JobSnapshot SceneAnalysisJob::snapshot() const {
  if (!impl_) return {};
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state;
}

std::optional<JobSnapshot> SceneAnalysisJob::takeCompletion() {
  if (!impl_) return std::nullopt;
  std::optional<JobSnapshot> completion;
  std::thread finishedWorker;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->completion) return std::nullopt;
    completion = std::move(impl_->completion);
    impl_->completion.reset();
    if (impl_->state.state != JobState::Running && impl_->worker.joinable()) {
      finishedWorker = std::move(impl_->worker);
    }
  }
  if (finishedWorker.joinable()) finishedWorker.join();
  return completion;
}

bool SceneAnalysisJob::consumeChanged() {
  return impl_ &&
         impl_->changed.exchange(false, std::memory_order_acq_rel);
}

}  // namespace playback_video_analysis
