#include "audio/separation/job.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include "core/runtime_helpers.h"
#include "core/waitable_signal.h"

namespace audio_separation {

struct Job::Impl {
  Impl(Operation separationOperation, WakeNotifier ownerWakeNotifier)
      : operation(std::move(separationOperation)),
        ownerWake(std::move(ownerWakeNotifier)) {}

  mutable std::mutex mutex;
  std::condition_variable runCondition;
  std::thread worker;
  std::atomic<bool> cancelRequested{false};
  std::atomic<bool> suspensionRequested{false};
  WaitableSignal changed;
  Operation operation;
  WakeNotifier ownerWake;
  JobSnapshot state;
  std::optional<JobSnapshot> completion;
  std::shared_ptr<DiagnosticLog> activeDiagnosticLog;
  std::mutex interruptionMutex;
  ExecutionControl::Interrupt activeInterrupt;
  std::uint64_t activeInterruptToken = 0;

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

  bool waitUntilRunnable(
      const ExecutionControl::YieldResources& yieldResources) {
    std::unique_lock<std::mutex> lock(mutex);
    if (state.scheduling != JobSchedulingState::Running &&
        !cancelRequested.load(std::memory_order_relaxed) &&
        yieldResources) {
      lock.unlock();
      yieldResources();
      lock.lock();
    }
    if (state.scheduling != JobSchedulingState::Running &&
        !cancelRequested.load(std::memory_order_relaxed)) {
      state.scheduling = JobSchedulingState::Suspended;
      notifyChanged();
    }
    runCondition.wait(lock, [this]() {
      return state.scheduling == JobSchedulingState::Running ||
             cancelRequested.load(std::memory_order_relaxed);
    });
    return !cancelRequested.load(std::memory_order_relaxed);
  }

  std::function<void()> registerInterrupt(
      ExecutionControl::Interrupt interrupt) {
    std::lock_guard<std::mutex> lock(interruptionMutex);
    const std::uint64_t token = ++activeInterruptToken;
    activeInterrupt = std::move(interrupt);
    if (activeInterrupt &&
        (cancelRequested.load(std::memory_order_relaxed) ||
         suspensionRequested.load(std::memory_order_relaxed))) {
      activeInterrupt();
    }
    return [this, token]() {
      std::lock_guard<std::mutex> lock(interruptionMutex);
      if (activeInterruptToken == token) activeInterrupt = {};
    };
  }

  void interruptActiveOperation() {
    std::lock_guard<std::mutex> lock(interruptionMutex);
    if (activeInterrupt) activeInterrupt();
  }

  bool beginCommit() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (state.state != JobState::Running ||
          cancelRequested.load(std::memory_order_acquire)) {
        return false;
      }
      state.state = JobState::Publishing;
      state.phase = "Publishing audio stems";
      state.scheduling = JobSchedulingState::Running;
      suspensionRequested.store(false, std::memory_order_release);
    }
    notifyChanged();
    return true;
  }

  void finish(bool succeeded, std::string error,
              const std::shared_ptr<DiagnosticLog>& diagnosticLog) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (succeeded && state.state != JobState::Publishing) {
        succeeded = false;
        error = "The audio-separation backend completed without publishing "
                "through the commit barrier.";
      }
    }
    if (diagnosticLog) {
      if (succeeded) {
        diagnosticLog->append(DiagnosticLevel::Info, "job",
                              "Audio separation completed successfully.");
      } else if (cancelRequested.load(std::memory_order_relaxed)) {
        diagnosticLog->append(DiagnosticLevel::Info, "job",
                              "Audio separation was cancelled.");
      } else {
        diagnosticLog->append(
            DiagnosticLevel::Error, "job",
            error.empty() ? "Audio separation failed unexpectedly." : error);
      }
    }
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
      state.scheduling = JobSchedulingState::Running;
      suspensionRequested.store(false, std::memory_order_relaxed);
      completion = state;
      activeDiagnosticLog.reset();
    }
    notifyChanged();
  }

  void run(std::filesystem::path mediaPath, ArtifactPaths outputPaths,
           std::shared_ptr<DiagnosticLog> diagnosticLog) {
    std::string error;
    bool succeeded = false;
    if (diagnosticLog) {
      diagnosticLog->append(DiagnosticLevel::Info, "job",
                            "Source: " + toUtf8String(mediaPath));
      for (const std::filesystem::path& output : outputPaths) {
        diagnosticLog->append(DiagnosticLevel::Info, "job",
                              "Output: " + toUtf8String(output));
      }
    }
    try {
      int lastLoggedPercent = -5;
      std::string lastLoggedPhase;
      const ExecutionControl control(
          &cancelRequested,
          [this](const ExecutionControl::YieldResources& yieldResources) {
            return waitUntilRunnable(yieldResources);
          },
          [this](ExecutionControl::Interrupt interrupt) {
            return registerInterrupt(std::move(interrupt));
          });
      if (control.checkpoint()) {
        succeeded = operation(
            mediaPath, outputPaths,
            [this, diagnosticLog, lastLoggedPercent,
             lastLoggedPhase = std::move(lastLoggedPhase)](
                float fraction, std::string phase) mutable {
              if (diagnosticLog) {
                const int percent = static_cast<int>(
                    std::clamp(fraction, 0.0f, 1.0f) * 100.0f);
                if (phase != lastLoggedPhase ||
                    percent >= lastLoggedPercent + 5) {
                  diagnosticLog->append(
                      DiagnosticLevel::Info, "progress",
                      std::to_string(percent) + "% " + phase);
                  lastLoggedPercent = percent;
                  lastLoggedPhase = phase;
                }
              }
              updateProgress(fraction, std::move(phase));
            },
            [diagnosticLog](DiagnosticLevel level, std::string_view component,
                            std::string_view message) {
              if (diagnosticLog) {
                diagnosticLog->append(level, component, message);
              }
            },
            control, [this]() { return beginCommit(); }, &error);
      }
    } catch (const std::exception& exception) {
      error = std::string("Audio separation failed: ") + exception.what();
    } catch (...) {
      error = "Audio separation failed unexpectedly.";
    }
    finish(succeeded, std::move(error), diagnosticLog);
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

bool Job::tryStart(const std::filesystem::path& mediaPath,
                   JobStartOptions options) {
  if (!impl_) return false;
  const ArtifactPaths outputPaths = artifactPathsFor(mediaPath);
  if (mediaPath.empty() || outputPaths.front().empty()) return false;

  impl_->joinFinishedWorker();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->operation || impl_->state.running() || impl_->completion) {
      return false;
    }
    std::string diagnosticError;
    std::shared_ptr<DiagnosticLog> diagnosticLog =
        createDiagnosticLog("audio-separation", &diagnosticError);
    impl_->cancelRequested.store(false, std::memory_order_relaxed);
    impl_->suspensionRequested.store(options.initiallyPaused,
                                     std::memory_order_relaxed);
    impl_->state = JobSnapshot{};
    impl_->state.state = JobState::Running;
    impl_->state.phase = "Starting audio separation";
    impl_->state.sourceFile = mediaPath;
    impl_->state.outputFiles = outputPaths;
    impl_->state.scheduling =
        options.initiallyPaused ? JobSchedulingState::Suspended
                                : JobSchedulingState::Running;
    if (diagnosticLog) {
      impl_->state.diagnosticLog = diagnosticLog->path();
      if (options.initiallyPaused) {
        diagnosticLog->append(
            DiagnosticLevel::Info, "scheduler",
            "Started paused while foreground video is playing.");
      }
    }
    impl_->activeDiagnosticLog = diagnosticLog;

    try {
      impl_->worker = std::thread(
          [implementation = impl_.get(), mediaPath, outputPaths,
           diagnosticLog = std::move(diagnosticLog)]() mutable {
            implementation->run(std::move(mediaPath),
                                std::move(outputPaths),
                                std::move(diagnosticLog));
          });
    } catch (const std::exception& exception) {
      impl_->state.state = JobState::Failed;
      impl_->state.phase.clear();
      impl_->state.error =
          std::string("Could not start audio separation: ") +
          exception.what();
      if (!diagnosticError.empty()) {
        impl_->state.error += " Diagnostics: " + diagnosticError;
      }
      impl_->completion = impl_->state;
      impl_->activeDiagnosticLog.reset();
    } catch (...) {
      impl_->state.state = JobState::Failed;
      impl_->state.phase.clear();
      impl_->state.error = "Could not start audio separation.";
      if (!diagnosticError.empty()) {
        impl_->state.error += " Diagnostics: " + diagnosticError;
      }
      impl_->completion = impl_->state;
      impl_->activeDiagnosticLog.reset();
    }
  }
  impl_->notifyChanged();
  return true;
}

bool Job::requestCancel() {
  if (!impl_) return false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->state.cancellable()) return false;
    impl_->cancelRequested.store(true, std::memory_order_relaxed);
    impl_->state.state = JobState::Cancelling;
    impl_->state.phase = "Cancelling audio separation";
    impl_->state.scheduling = JobSchedulingState::Running;
    impl_->suspensionRequested.store(false, std::memory_order_relaxed);
  }
  impl_->interruptActiveOperation();
  impl_->runCondition.notify_all();
  impl_->notifyChanged();
  return true;
}

bool Job::setPaused(bool paused) {
  if (!impl_) return false;
  std::shared_ptr<DiagnosticLog> diagnosticLog;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state.state != JobState::Running) {
      return false;
    }
    if (paused) {
      if (impl_->state.scheduling != JobSchedulingState::Running) {
        return false;
      }
      impl_->state.scheduling = JobSchedulingState::Suspending;
      impl_->suspensionRequested.store(true, std::memory_order_relaxed);
    } else {
      if (impl_->state.scheduling == JobSchedulingState::Running) {
        return false;
      }
      impl_->state.scheduling = JobSchedulingState::Running;
      impl_->suspensionRequested.store(false, std::memory_order_relaxed);
    }
    diagnosticLog = impl_->activeDiagnosticLog;
  }
  if (diagnosticLog) {
    diagnosticLog->append(
        DiagnosticLevel::Info, "scheduler",
        paused ? "Suspending for foreground video playback."
               : "Resumed after foreground video playback yielded.");
  }
  if (paused) {
    impl_->interruptActiveOperation();
  } else {
    impl_->runCondition.notify_all();
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

bool Job::configured() const {
  if (!impl_) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return static_cast<bool>(impl_->operation);
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
