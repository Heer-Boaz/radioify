#include "playback/video/edit/export.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "exporter_job_tests: " << message << '\n';
  return false;
}

class ControlledExport {
 public:
  playback_video_edit::ExportResult run(
      const playback_video_edit::ExportRequest& request,
      const std::atomic<bool>* cancelled,
      const playback_video_edit::Exporter::ProgressReporter& reportProgress) {
    int invocation = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      invocation = ++invocations_;
      lastRequest_ = request;
    }

    reportProgress(invocation == 1 ? 0.4 : 0.2);
    {
      std::unique_lock<std::mutex> lock(mutex_);
      reported_ = std::max(reported_, invocation);
      changed_.notify_all();
      changed_.wait(lock, [&]() { return released_ >= invocation; });
    }

    if (invocation == 1) {
      return {playback_video_edit::ExportState::Succeeded, "h264_nvenc", {}};
    }
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
      return {playback_video_edit::ExportState::Failed, {},
              "Controlled cancellation."};
    }
    if (invocation == 3) {
      return {playback_video_edit::ExportState::Failed, {},
              "Controlled export failure."};
    }
    return {playback_video_edit::ExportState::Running, {}, {}};
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

  playback_video_edit::ExportRequest lastRequest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastRequest_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  int invocations_ = 0;
  int reported_ = 0;
  int released_ = 0;
  playback_video_edit::ExportRequest lastRequest_;
};

bool waitUntilFinished(playback_video_edit::Exporter& exporter) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (exporter.snapshot().finished()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return exporter.snapshot().finished();
}

playback_video_edit::ExportRequest requestFor(
    const std::filesystem::path& destination) {
  playback_video_edit::ExportRequest request;
  request.sourcePath = "source.mp4";
  request.destinationPath = destination;
  request.decisions.keptRanges.push_back({1'000'000, 9'000'000});
  request.videoStreamIndex = 2;
  return request;
}

}  // namespace

int main() {
  namespace edit = playback_video_edit;
  bool ok = true;
  ControlledExport controlled;
  edit::Exporter exporter(
      [&](const edit::ExportRequest& request,
          const std::atomic<bool>* cancelled,
          const edit::Exporter::ProgressReporter& reportProgress) {
        return controlled.run(request, cancelled, reportProgress);
      });

  ok &= expect(!exporter.start({}),
               "an incomplete request must not start an export");
  const edit::ExportRequest firstRequest = requestFor("edited.mp4");
  ok &= expect(exporter.start(firstRequest) &&
                   controlled.waitUntilReported(1),
               "a valid request must start the injected export");
  const edit::ExportSnapshot running = exporter.snapshot();
  ok &= expect(running.running() && running.progress == 0.4 &&
                   running.destinationPath == firstRequest.destinationPath &&
                   running.decisions == firstRequest.decisions,
               "running snapshots must retain coherent request and progress");
  const edit::ExportRequest received = controlled.lastRequest();
  ok &= expect(received.sourcePath == firstRequest.sourcePath &&
                   received.destinationPath == firstRequest.destinationPath &&
                   received.decisions == firstRequest.decisions &&
                   received.videoStreamIndex == firstRequest.videoStreamIndex,
               "the job must pass its immutable request to the backend");
  ok &= expect(!exporter.start(requestFor("other.mp4")),
               "a running export must reject another request");

  controlled.release(1);
  ok &= expect(waitUntilFinished(exporter),
               "the successful export must reach a terminal state");
  ok &= expect(!exporter.start(requestFor("other.mp4")),
               "an unconsumed completion must not be overwritten");
  const auto succeeded = exporter.takeCompletion();
  ok &= expect(succeeded &&
                   succeeded->state == edit::ExportState::Succeeded &&
                   succeeded->progress == 1.0 &&
                   succeeded->destinationPath == firstRequest.destinationPath &&
                   succeeded->decisions == firstRequest.decisions &&
                   succeeded->videoEncoder == "h264_nvenc",
               "successful completion must publish its exact revision once");
  ok &= expect(!exporter.takeCompletion(),
               "export completion must be consumed exactly once");

  const edit::ExportRequest cancelledRequest = requestFor("cancelled.mp4");
  ok &= expect(exporter.start(cancelledRequest) &&
                   controlled.waitUntilReported(2),
               "a consumed completion must permit another export");
  ok &= expect(exporter.cancel() && !exporter.cancel(),
               "export cancellation must be accepted exactly once");
  controlled.release(2);
  ok &= expect(waitUntilFinished(exporter),
               "the cancelled export must reach a terminal state");
  const auto cancelled = exporter.takeCompletion();
  ok &= expect(cancelled &&
                   cancelled->state == edit::ExportState::Cancelled &&
                   cancelled->error.empty() &&
                   cancelled->destinationPath ==
                       cancelledRequest.destinationPath,
               "cancelled work must retain identity without backend failure");

  const edit::ExportRequest failedRequest = requestFor("failed.mp4");
  ok &= expect(exporter.start(failedRequest) &&
                   controlled.waitUntilReported(3),
               "the job must remain reusable after cancellation");
  controlled.release(3);
  ok &= expect(waitUntilFinished(exporter),
               "the failed export must reach a terminal state");
  const auto failed = exporter.takeCompletion();
  ok &= expect(failed && failed->state == edit::ExportState::Failed &&
                   failed->error == "Controlled export failure.",
               "export failures must retain backend detail");

  ok &= expect(exporter.start(requestFor("invalid-result.mp4")) &&
                   controlled.waitUntilReported(4),
               "the job must remain reusable after failure");
  controlled.release(4);
  ok &= expect(waitUntilFinished(exporter),
               "a malformed backend result must become terminal");
  const auto malformed = exporter.takeCompletion();
  ok &= expect(malformed && malformed->state == edit::ExportState::Failed &&
                   malformed->error ==
                       "Export backend returned a non-terminal result.",
               "the worker boundary must reject non-terminal backend results");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
