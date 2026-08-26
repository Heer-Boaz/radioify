#include "playback/video/transcript/generation_job.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "subtitle_generation_job_tests: " << message << '\n';
  return false;
}

class ControlledOperation {
 public:
  bool run(
      const std::filesystem::path& source,
      const std::filesystem::path& output,
      const playback_video_transcript::GenerationJob::ProgressReporter&
          reportProgress,
      const std::atomic<bool>* cancelRequested, std::string* error) {
    int invocation = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      invocation = ++invocations_;
      source_ = source;
      output_ = output;
    }

    reportProgress(invocation == 1 ? 0.35f : 0.2f,
                   invocation == 1 ? "Transcribing audio"
                                   : "Opening video audio");
    {
      std::unique_lock<std::mutex> lock(mutex_);
      reported_ = std::max(reported_, invocation);
      changed_.notify_all();
      changed_.wait(lock, [&]() { return released_ >= invocation; });
    }

    if (invocation == 1) return true;
    if (cancelRequested &&
        cancelRequested->load(std::memory_order_relaxed)) {
      if (error) *error = "Controlled cancellation.";
      return false;
    }
    if (error) *error = "Controlled generation failure.";
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

  std::filesystem::path source() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_;
  }

  std::filesystem::path output() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return output_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  int invocations_ = 0;
  int reported_ = 0;
  int released_ = 0;
  std::filesystem::path source_;
  std::filesystem::path output_;
};

std::optional<playback_video_transcript::GenerationJobSnapshot>
waitForCompletion(playback_video_transcript::GenerationJob& job) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (auto completion = job.takeCompletion()) return completion;
    const DWORD waitResult = WaitForSingleObject(
        static_cast<HANDLE>(job.nativeWaitHandle().get()), 2000);
    if (waitResult != WAIT_OBJECT_0) return std::nullopt;
    job.consumeChanged();
  }
  return job.takeCompletion();
}

}  // namespace

int main() {
  namespace transcript = playback_video_transcript;
  bool ok = true;
  ControlledOperation controlled;
  transcript::GenerationJob job(
      [&](const std::filesystem::path& source,
          const std::filesystem::path& output,
          const transcript::GenerationJob::ProgressReporter& reportProgress,
          const std::atomic<bool>* cancelRequested, std::string* error) {
        return controlled.run(source, output, reportProgress, cancelRequested,
                              error);
      });

  ok &= expect(static_cast<bool>(job.nativeWaitHandle()),
               "the job must expose a valid activity handle");
  ok &= expect(!job.tryStart({}) &&
                   job.snapshot().state == transcript::GenerationJobState::Idle,
               "an empty request must not change idle state");

  ok &= expect(job.tryStart("clip.mp4"),
               "a valid request must start the injected operation");
  ok &= expect(controlled.waitUntilReported(1),
               "the first operation must publish progress");
  const transcript::GenerationJobSnapshot running = job.snapshot();
  ok &= expect(running.state == transcript::GenerationJobState::Running &&
                   running.running() && !running.finished() &&
                   running.progress == 0.35f &&
                   running.phase == "Transcribing audio",
               "running state must be coherent and retain backend progress");
  ok &= expect(controlled.source() == std::filesystem::path("clip.mp4") &&
                   controlled.output() ==
                       std::filesystem::path("clip.transcript.srt"),
               "the job must own canonical output-path selection");
  ok &= expect(!job.tryStart("other.mp4"),
               "a running job must reject a second request");

  controlled.release(1);
  const auto succeeded = waitForCompletion(job);
  ok &= expect(succeeded && succeeded->succeeded() && succeeded->finished() &&
                   succeeded->progress == 1.0f,
               "successful completion must be terminal and complete");
  ok &= expect(!job.takeCompletion(),
               "each completion must be consumed exactly once");

  ok &= expect(job.tryStart("cancel.mp4") &&
                   controlled.waitUntilReported(2),
               "a consumed completion must permit another request");
  ok &= expect(job.requestCancel() && !job.requestCancel(),
               "cancellation must be accepted exactly once");
  const transcript::GenerationJobSnapshot cancelling = job.snapshot();
  ok &= expect(cancelling.cancelling() && cancelling.running() &&
                   cancelling.phase == "Cancelling subtitle generation",
               "cancellation must be an explicit running state");
  controlled.release(2);
  const auto cancelled = waitForCompletion(job);
  ok &= expect(cancelled &&
                   cancelled->state ==
                       transcript::GenerationJobState::Cancelled &&
                   cancelled->error.empty(),
               "cancelled work must not surface a backend failure");

  ok &= expect(job.tryStart("failure.mp4") &&
                   controlled.waitUntilReported(3),
               "the job must remain reusable after cancellation");
  controlled.release(3);
  const auto failed = waitForCompletion(job);
  ok &= expect(failed &&
                   failed->state == transcript::GenerationJobState::Failed &&
                   failed->error == "Controlled generation failure.",
               "backend failures must remain typed and retain their detail");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
