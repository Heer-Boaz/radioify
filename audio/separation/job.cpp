#include "audio/separation/job.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include "core/waitable_signal.h"

namespace audio_separation {

struct Job::Impl {
  Impl(Operation separationOperation, WakeNotifier ownerWakeNotifier)
      : operation(std::move(separationOperation)),
        ownerWake(std::move(ownerWakeNotifier)) {}

  mutable std::mutex mutex;
  std::thread worker;
  std::atomic<bool> cancelRequested{false};
  WaitableSignal changed;
  Operation operation;
  WakeNotifier ownerWake;
  JobSnapshot state;
  std::optional<JobSnapshot> completion;

  void notifyChanged() {
    changed.signal();
    ownerWake.notify();
  }

  void updateProgress(float fraction, std::string phase) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!state.running()) return;
      state.progress =
          std::max(state.progress, std::clamp(fraction, 0.0f, 1.0f));
      if (!state.cancelling()) state.phase = std::move(phase);
    }
    notifyChanged();
  }

  void finish(bool succeeded, std::string error) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (succeeded) {
        state.state = JobState::Succeeded;
        state.progress = 1.0f;
        state.error.clear();
      } else if (cancelRequested.load(std::memory_order_relaxed)) {
        state.state = JobState::Cancelled;
        state.error.clear();
      } else {
        state.state = JobState::Failed;
        state.error = error.empty()
                          ? "Audio separation failed unexpectedly."
                          : std::move(error);
      }
      state.phase.clear();
      completion = state;
    }
    notifyChanged();
  }

  void run(std::filesystem::path mediaPath, ArtifactPaths outputPaths) {
    std::string error;
    bool succeeded = false;
    try {
      succeeded = operation(
          mediaPath, outputPaths,
          [this](float fraction, std::string phase) {
            updateProgress(fraction, std::move(phase));
          },
          &cancelRequested, &error);
    } catch (const std::exception& exception) {
      error = std::string("Audio separation failed: ") + exception.what();
    } catch (...) {
      error = "Audio separation failed unexpectedly.";
    }
    finish(succeeded, std::move(error));
  }

  void joinFinishedWorker() {
    std::thread finishedWorker;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!state.running() && worker.joinable()) {
        finishedWorker = std::move(worker);
      }
    }
    if (finishedWorker.joinable()) finishedWorker.join();
  }
};

Job::Job(Operation operation)
    : Job(std::move(operation), WakeNotifier{}) {}

Job::Job(Operation operation, WakeNotifier ownerWake)
    : impl_(std::make_unique<Impl>(std::move(operation),
                                  std::move(ownerWake))) {}

Job::~Job() { cancelAndJoin(); }

bool Job::tryStart(const std::filesystem::path& mediaPath) {
  if (!impl_) return false;
  const ArtifactPaths outputPaths = artifactPathsFor(mediaPath);
  if (mediaPath.empty() || outputPaths.front().empty()) return false;

  impl_->joinFinishedWorker();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->operation || impl_->state.running() || impl_->completion) {
      return false;
    }
    impl_->cancelRequested.store(false, std::memory_order_relaxed);
    impl_->state = JobSnapshot{};
    impl_->state.state = JobState::Running;
    impl_->state.phase = "Starting audio separation";
    impl_->state.sourceFile = mediaPath;
    impl_->state.outputFiles = outputPaths;

    try {
      impl_->worker = std::thread(
          [implementation = impl_.get(), mediaPath, outputPaths]() mutable {
            implementation->run(std::move(mediaPath),
                                std::move(outputPaths));
          });
    } catch (const std::exception& exception) {
      impl_->state.state = JobState::Failed;
      impl_->state.phase.clear();
      impl_->state.error =
          std::string("Could not start audio separation: ") +
          exception.what();
      impl_->completion = impl_->state;
    } catch (...) {
      impl_->state.state = JobState::Failed;
      impl_->state.phase.clear();
      impl_->state.error = "Could not start audio separation.";
      impl_->completion = impl_->state;
    }
  }
  impl_->notifyChanged();
  return true;
}

bool Job::requestCancel() {
  if (!impl_) return false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state.state != JobState::Running) return false;
    impl_->cancelRequested.store(true, std::memory_order_relaxed);
    impl_->state.state = JobState::Cancelling;
    impl_->state.phase = "Cancelling audio separation";
  }
  impl_->notifyChanged();
  return true;
}

void Job::cancelAndJoin() {
  if (!impl_) return;
  requestCancel();
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->worker.joinable()) worker = std::move(impl_->worker);
  }
  if (worker.joinable()) worker.join();
}

JobSnapshot Job::snapshot() const {
  if (!impl_) return {};
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state;
}

std::optional<JobSnapshot> Job::takeCompletion() {
  if (!impl_) return std::nullopt;
  std::optional<JobSnapshot> completion;
  std::thread finishedWorker;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->completion) return std::nullopt;
    completion = std::move(impl_->completion);
    impl_->completion.reset();
    if (!impl_->state.running() && impl_->worker.joinable()) {
      finishedWorker = std::move(impl_->worker);
    }
  }
  if (finishedWorker.joinable()) finishedWorker.join();
  return completion;
}

bool Job::consumeChanged() {
  return impl_ && impl_->changed.consume();
}

NativeWaitHandle Job::nativeWaitHandle() const {
  return impl_ ? impl_->changed.nativeWaitHandle() : NativeWaitHandle{};
}

}  // namespace audio_separation
