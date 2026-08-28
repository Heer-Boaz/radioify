#include "audio/separation/job.h"
#include "audio/separation/operation.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include "runtime_helpers.h"

namespace {

using Clock = std::chrono::steady_clock;

bool gpuReady(const audio_separation::JobSnapshot& snapshot) {
  return snapshot.phase == "DirectML GPU ready";
}

bool modelRestoreStarted(const audio_separation::JobSnapshot& snapshot) {
  return snapshot.phase == "Restoring DirectML separation model";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: audio_separation_scheduler_smoke <media-file> "
                 "[model-file]\n";
    return 2;
  }

  const std::filesystem::path media = pathFromUtf8String(argv[1]);
  const audio_separation::Job::Operation operation =
      argc == 3
          ? audio_separation::makeModelOperation(pathFromUtf8String(argv[2]))
          : audio_separation::makeProductionOperation();
  if (!operation) {
    std::cerr << "Audio separation scheduler smoke failed: backend is not "
                 "configured.\n";
    return EXIT_FAILURE;
  }

  audio_separation::Job job(operation);
  if (!job.tryStart(media)) {
    std::cerr << "Audio separation scheduler smoke failed: job did not "
                 "start.\n";
    return EXIT_FAILURE;
  }

  constexpr auto kOverallTimeout = std::chrono::minutes(3);
  constexpr auto kTransitionTimeout = std::chrono::seconds(30);
  const auto started = Clock::now();
  std::optional<Clock::time_point> suspensionRequestedAt;
  std::optional<Clock::time_point> resumeRequestedAt;
  bool suspended = false;
  bool restoreStarted = false;
  bool cancellationRequested = false;
  std::optional<audio_separation::JobSnapshot> completion;

  while (Clock::now() - started < kOverallTimeout) {
    const audio_separation::JobSnapshot snapshot = job.snapshot();
    if (!suspensionRequestedAt && gpuReady(snapshot)) {
      if (!job.setPaused(true)) {
        std::cerr << "Scheduler smoke failed: suspension was rejected.\n";
        break;
      }
      suspensionRequestedAt = Clock::now();
      std::cout << "Suspension requested during DirectML processing.\n";
    }

    if (suspensionRequestedAt && !suspended &&
        snapshot.scheduling ==
            audio_separation::JobSchedulingState::Suspended) {
      suspended = true;
      const double seconds = std::chrono::duration<double>(
                                 Clock::now() - *suspensionRequestedAt)
                                 .count();
      std::cout << "DirectML resources suspended in " << seconds << " s.\n";
      if (!job.setPaused(false)) {
        std::cerr << "Scheduler smoke failed: resume was rejected.\n";
        break;
      }
      resumeRequestedAt = Clock::now();
    }

    if (resumeRequestedAt && !restoreStarted && modelRestoreStarted(snapshot)) {
      restoreStarted = true;
      std::cout << "DirectML resource restoration started.\n";
      cancellationRequested = job.requestCancel();
    }

    if (suspensionRequestedAt && !suspended &&
        Clock::now() - *suspensionRequestedAt > kTransitionTimeout) {
      std::cerr << "Scheduler smoke failed: suspension timed out.\n";
      break;
    }
    if (resumeRequestedAt && !restoreStarted &&
        Clock::now() - *resumeRequestedAt > kTransitionTimeout) {
      std::cerr << "Scheduler smoke failed: resource restoration timed out.\n";
      break;
    }

    completion = job.takeCompletion();
    if (completion) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (!completion) {
    job.requestCancel();
    job.cancelAndJoin();
    completion = job.takeCompletion();
  }

  const bool passed = suspended && restoreStarted && cancellationRequested &&
                      completion &&
                      completion->state ==
                          audio_separation::JobState::Cancelled;
  if (!passed) {
    std::cerr << "Audio separation scheduler smoke failed.";
    if (completion && !completion->error.empty()) {
      std::cerr << ' ' << completion->error;
    }
    std::cerr << '\n';
    return EXIT_FAILURE;
  }

  std::cout << "Audio separation scheduler smoke passed.\n";
  if (!completion->diagnosticLog.empty()) {
    std::cout << "Diagnostics: "
              << toUtf8String(completion->diagnosticLog) << '\n';
  }
  return EXIT_SUCCESS;
}
