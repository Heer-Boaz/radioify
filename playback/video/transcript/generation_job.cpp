#include "playback/video/transcript/generation_job.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include "core/waitable_signal.h"
#include "playback/video/transcript/artifact.h"

namespace playback_video_transcript {

struct GenerationJob::Impl {
  Impl(Operation generationOperation, WakeNotifier ownerWakeNotifier)
      : operation(std::move(generationOperation)),
        ownerWake(std::move(ownerWakeNotifier)) {}

  mutable std::mutex mutex;
  std::thread worker;
  std::atomic<bool> cancelRequested{false};
  WaitableSignal changed;
  Operation operation;
  WakeNotifier ownerWake;
  GenerationJobSnapshot state;
  std::optional<GenerationJobSnapshot> completion;

  void notifyChanged() {
    changed.signal();
    ownerWake.notify();
  }

  void updateProgress(float fraction, std::string phase) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!state.running()) return;
      state.progress = std::max(
          state.progress, std::clamp(fraction, 0.0f, 1.0f));
      if (!state.cancelling()) state.phase = std::move(phase);
    }
    notifyChanged();
  }

  bool beginCommit() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (state.state != GenerationJobState::Running ||
          cancelRequested.load(std::memory_order_acquire)) {
        return false;
      }
      state.state = GenerationJobState::Publishing;
      state.phase = "Publishing subtitles";
    }
    notifyChanged();
    return true;
  }

  void finish(bool succeeded, std::string error) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (succeeded && state.state != GenerationJobState::Publishing) {
        succeeded = false;
        error = "The subtitle backend completed without publishing through "
                "the commit barrier.";
      }
      if (succeeded) {
        state.state = GenerationJobState::Succeeded;
        state.progress = 1.0f;
        state.error.clear();
        state.diagnosticError.clear();
      } else if (cancelRequested.load(std::memory_order_relaxed)) {
        state.state = GenerationJobState::Cancelled;
        state.diagnosticError = std::move(error);
        state.error.clear();
      } else {
        state.state = GenerationJobState::Failed;
        state.diagnosticError.clear();
        state.error = error.empty() ? "Subtitle generation failed unexpectedly."
                                    : std::move(error);
      }
      state.phase.clear();
      completion = state;
    }
    notifyChanged();
  }

  void run(std::filesystem::path videoPath,
           std::filesystem::path outputPath) {
    std::string error;
    bool succeeded = false;
    try {
      succeeded = operation(
          videoPath, outputPath,
          [this](float fraction, std::string phase) {
            updateProgress(fraction, std::move(phase));
          },
          &cancelRequested, [this]() { return beginCommit(); }, &error);
    } catch (const std::exception& exception) {
      error = std::string("Subtitle generation failed: ") + exception.what();
    } catch (...) {
      error = "Subtitle generation failed unexpectedly.";
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

GenerationJob::GenerationJob(Operation operation)
    : GenerationJob(std::move(operation), WakeNotifier{}) {}

GenerationJob::GenerationJob(Operation operation, WakeNotifier ownerWake)
    : impl_(std::make_unique<Impl>(std::move(operation),
                                  std::move(ownerWake))) {}

GenerationJob::~GenerationJob() { cancelAndJoin(); }

bool GenerationJob::tryStart(const std::filesystem::path& videoPath) {
  if (!impl_) return false;
  const std::filesystem::path outputPath = transcriptPathForVideo(videoPath);
  if (videoPath.empty() || outputPath.empty()) return false;

  impl_->joinFinishedWorker();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->operation || impl_->state.running() || impl_->completion) {
      return false;
    }
    impl_->cancelRequested.store(false, std::memory_order_relaxed);
    impl_->state = GenerationJobSnapshot{};
    impl_->state.state = GenerationJobState::Running;
    impl_->state.phase = "Starting subtitle generation";
    impl_->state.sourceFile = videoPath;
    impl_->state.outputFile = outputPath;

    try {
      impl_->worker = std::thread(
          [implementation = impl_.get(), videoPath, outputPath]() mutable {
            implementation->run(std::move(videoPath), std::move(outputPath));
          });
    } catch (const std::exception& exception) {
      impl_->state.state = GenerationJobState::Failed;
      impl_->state.phase.clear();
      impl_->state.error =
          std::string("Could not start subtitle generation: ") +
          exception.what();
      impl_->completion = impl_->state;
    } catch (...) {
      impl_->state.state = GenerationJobState::Failed;
      impl_->state.phase.clear();
      impl_->state.error = "Could not start subtitle generation.";
      impl_->completion = impl_->state;
    }
  }
  impl_->notifyChanged();
  return true;
}

bool GenerationJob::requestCancel() {
  if (!impl_) return false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->state.cancellable()) return false;
    impl_->cancelRequested.store(true, std::memory_order_relaxed);
    impl_->state.state = GenerationJobState::Cancelling;
    impl_->state.phase = "Cancelling subtitle generation";
  }
  impl_->notifyChanged();
  return true;
}

void GenerationJob::cancelAndJoin() {
  if (!impl_) return;
  requestCancel();
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->worker.joinable()) worker = std::move(impl_->worker);
  }
  if (worker.joinable()) worker.join();
}

GenerationJobSnapshot GenerationJob::snapshot() const {
  if (!impl_) return {};
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state;
}

bool GenerationJob::configured() const {
  if (!impl_) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return static_cast<bool>(impl_->operation);
}

std::optional<GenerationJobSnapshot> GenerationJob::takeCompletion() {
  if (!impl_) return std::nullopt;
  std::optional<GenerationJobSnapshot> completion;
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

bool GenerationJob::consumeChanged() {
  return impl_ && impl_->changed.consume();
}

NativeWaitHandle GenerationJob::nativeWaitHandle() const {
  return impl_ ? impl_->changed.nativeWaitHandle() : NativeWaitHandle{};
}

}  // namespace playback_video_transcript
