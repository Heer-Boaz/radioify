#include "playback/video/analysis/scene_analysis_job.h"
#include "playback/video/analysis/scene_analyzer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "scene_analysis_job_tests: " << message << '\n';
  return false;
}

DWORD waitNow(NativeWaitHandle handle) {
  return WaitForSingleObject(static_cast<HANDLE>(handle.get()), 0);
}

class ControlledAnalysis {
 public:
  bool run(
      const playback_video_analysis::JobRequest& request,
      const playback_video_analysis::SceneAnalysisJob::ProgressReporter&
          reportProgress,
      const std::atomic<bool>* cancelled,
      playback_video_analysis::AnalysisResult* result, std::string* error) {
    int invocation = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      invocation = ++invocations_;
      lastRequest_ = request;
    }

    reportProgress({invocation == 1 ? 0.4 : 0.2,
                    invocation == 1 ? "Sampling video"
                                    : "Reading generated subtitles"});
    {
      std::unique_lock<std::mutex> lock(mutex_);
      reported_ = std::max(reported_, invocation);
      changed_.notify_all();
      changed_.wait(lock, [&]() { return released_ >= invocation; });
    }

    if (invocation == 1) {
      if (result) {
        result->durationUs = request.durationUs;
        result->visualSampleCount = 42;
        result->transcriptPath = request.sourcePath;
        result->transcriptPath.replace_extension(".transcript.srt");
        result->suggestions.push_back(
            {7, 10'000'000, 20'000'000,
             playback_video_analysis::SceneKind::Cutscene, 0.84f, {}});
      }
      return true;
    }
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
      if (error) *error = "Controlled cancellation.";
      return false;
    }
    if (error) *error = "Controlled analysis failure.";
    return false;
  }

  bool waitUntilReported(int invocation) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(2),
                             [&]() { return reported_ >= invocation; });
  }

  void release(int invocation) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = std::max(released_, invocation);
    }
    changed_.notify_all();
  }

  playback_video_analysis::JobRequest lastRequest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastRequest_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  int invocations_ = 0;
  int reported_ = 0;
  int released_ = 0;
  playback_video_analysis::JobRequest lastRequest_;
};

bool waitUntilFinished(playback_video_analysis::SceneAnalysisJob& job) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (job.snapshot().finished() && job.stopReady()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return job.snapshot().finished() && job.stopReady();
}

bool waitUntilStopReady(playback_video_analysis::SceneAnalysisJob& job) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (job.stopReady()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return job.stopReady();
}

}  // namespace

int main() {
  namespace analysis = playback_video_analysis;
  bool ok = true;
  ControlledAnalysis controlled;
  WakeEvent ownerWake;
  analysis::SceneAnalysisJob job(
      [&](const analysis::JobRequest& request,
          const analysis::SceneAnalysisJob::ProgressReporter& reportProgress,
          const std::atomic<bool>* cancelled,
          analysis::AnalysisResult* result, std::string* error) {
        return controlled.run(request, reportProgress, cancelled, result,
                              error);
      },
      ownerWake.notifier());

  const NativeWaitHandle changeHandle = job.nativeWaitHandle();
  const NativeWaitHandle ownerWakeHandle = ownerWake.nativeWaitHandle();
  ok &= expect(changeHandle && ownerWakeHandle &&
                   waitNow(changeHandle) == WAIT_TIMEOUT &&
                   waitNow(ownerWakeHandle) == WAIT_TIMEOUT,
               "a new analysis job must expose an unsignaled wake handle");

  analysis::JobRequest request;
  request.sourcePath = "clip.mp4";
  request.durationUs = 60'000'000;
  request.videoStreamIndex = 2;
  ok &= expect(!job.start({}),
               "an incomplete request must not start analysis");
  ok &= expect(job.start(request) && controlled.waitUntilReported(1),
               "a valid request must start the injected analysis");
  ok &= expect(job.workerWaitHandle() &&
                   waitNow(job.workerWaitHandle()) == WAIT_TIMEOUT,
               "running analysis must expose its exact worker-exit handle");
  ok &= expect(waitNow(changeHandle) == WAIT_OBJECT_0 &&
                   waitNow(ownerWakeHandle) == WAIT_OBJECT_0 &&
                   job.consumeChanged() &&
                   ownerWake.consume() &&
                   waitNow(changeHandle) == WAIT_TIMEOUT,
               "analysis changes must wake both the job and its owner once");
  const analysis::JobSnapshot running = job.snapshot();
  ok &= expect(running.running() && running.progress == 0.4 &&
                   running.phase == "Sampling video",
               "running snapshots must retain coherent progress");
  const analysis::JobRequest received = controlled.lastRequest();
  ok &= expect(received.sourcePath == request.sourcePath &&
                   received.durationUs == request.durationUs &&
                   received.videoStreamIndex == request.videoStreamIndex,
               "the job must pass its immutable request to the backend");
  ok &= expect(!job.start(request),
               "a running analysis must reject another request");

  controlled.release(1);
  ok &= expect(waitUntilFinished(job),
               "the successful analysis must reach a terminal state");
  ok &= expect(waitNow(changeHandle) == WAIT_OBJECT_0 && job.consumeChanged(),
               "analysis completion must wake the session loop");
  ok &= expect(!job.start(request),
               "an unconsumed completion must not be overwritten");
  const auto succeeded = job.takeCompletion();
  ok &= expect(succeeded &&
                   succeeded->state == analysis::JobState::Succeeded &&
                   succeeded->visualSampleCount == 42 &&
                   succeeded->usedIndexedTranscript &&
                   succeeded->suggestions.size() == 1 &&
                   succeeded->suggestions.front().id == 7,
               "successful completion must publish the full analysis once");
  ok &= expect(!job.workerWaitHandle(),
               "consuming a ready completion must reclaim its worker");
  ok &= expect(!job.takeCompletion(),
               "analysis completion must be consumed exactly once");

  request.forceReanalysis = true;
  ok &= expect(job.start(request) && controlled.waitUntilReported(2),
               "a consumed completion must permit reanalysis");
  ok &= expect(job.consumeChanged(),
               "a subsequent analysis start must publish a change");
  ok &= expect(job.cancel() && !job.cancel(),
               "analysis cancellation must be accepted exactly once");
  ok &= expect(waitNow(changeHandle) == WAIT_OBJECT_0 && job.consumeChanged(),
               "analysis cancellation must wake the session loop");
  controlled.release(2);
  ok &= expect(waitUntilFinished(job),
               "cancelled analysis must reach a terminal state");
  const auto cancelled = job.takeCompletion();
  ok &= expect(cancelled && cancelled->state == analysis::JobState::Cancelled &&
                   cancelled->error.empty(),
               "cancelled analysis must not expose a backend failure");

  ok &= expect(job.start(request) && controlled.waitUntilReported(3),
               "the job must remain reusable after cancellation");
  controlled.release(3);
  ok &= expect(waitUntilFinished(job),
               "failed analysis must reach a terminal state");
  const auto failed = job.takeCompletion();
  ok &= expect(failed && failed->state == analysis::JobState::Failed &&
                   failed->error == "Controlled analysis failure.",
               "analysis failures must retain backend detail");

  ok &= expect(job.start(request) && controlled.waitUntilReported(4),
               "the job must start controlled shutdown work");
  job.requestStop();
  ok &= expect(!job.stopReady(),
               "requestStop must never join the analysis worker");
  controlled.release(4);
  ok &= expect(waitUntilStopReady(job),
               "analysis completion must publish stop readiness");
  ok &= expect(job.finishStop() && job.stopReady(),
               "finishStop must reclaim only an already completed worker");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
