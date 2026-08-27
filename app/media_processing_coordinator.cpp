#include "app/media_processing_coordinator.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include "audio/media_formats.h"
#include "audio/separation/artifact.h"
#include "core/path_identity.h"
#include "core/runtime_helpers.h"
#include "core/waitable_signal.h"

namespace media_processing {
namespace {

TaskOutcome outcomeFor(
    const playback_video_transcript::GenerationJobSnapshot& snapshot) {
  using State = playback_video_transcript::GenerationJobState;
  if (snapshot.state == State::Succeeded) return TaskOutcome::Succeeded;
  if (snapshot.state == State::Cancelled) return TaskOutcome::Cancelled;
  return TaskOutcome::Failed;
}

TaskOutcome outcomeFor(const audio_separation::JobSnapshot& snapshot) {
  using State = audio_separation::JobState;
  if (snapshot.state == State::Succeeded) return TaskOutcome::Succeeded;
  if (snapshot.state == State::Cancelled) return TaskOutcome::Cancelled;
  return TaskOutcome::Failed;
}

class WorkerTask {
 public:
  using ProgressReporter = std::function<void(float)>;
  using Operation = std::function<TaskCompletion(const ProgressReporter&)>;

  ~WorkerTask() { join(); }

  bool tryStart(TaskActivity activity, Operation operation) {
    joinFinished();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!operation || activity_ || completion_) return false;
      activity.progress = std::clamp(activity.progress, 0.0f, 1.0f);
      activity_ = std::move(activity);
      try {
        worker_ = std::thread([this, operation = std::move(operation)]() {
          run(operation);
        });
      } catch (const std::exception& exception) {
        TaskCompletion completion;
        completion.kind = activity_->kind;
        completion.outcome = TaskOutcome::Failed;
        completion.sourceFile = activity_->sourceFile;
        completion.detail =
            std::string("Could not start media processing: ") +
            exception.what();
        completion_ = std::move(completion);
        activity_.reset();
      } catch (...) {
        TaskCompletion completion;
        completion.kind = activity_->kind;
        completion.outcome = TaskOutcome::Failed;
        completion.sourceFile = activity_->sourceFile;
        completion.detail = "Could not start media processing.";
        completion_ = std::move(completion);
        activity_.reset();
      }
    }
    changed_.signal();
    return true;
  }

  bool running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activity_.has_value();
  }

  bool hasPendingCompletion() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completion_.has_value();
  }

  std::optional<TaskActivity> activity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activity_;
  }

  std::optional<TaskCompletion> takeCompletion() {
    std::optional<TaskCompletion> completion;
    std::thread finishedWorker;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!completion_) return std::nullopt;
      completion = std::move(completion_);
      completion_.reset();
      if (!activity_ && worker_.joinable()) {
        finishedWorker = std::move(worker_);
      }
    }
    if (finishedWorker.joinable()) finishedWorker.join();
    return completion;
  }

  bool consumeChanged() { return changed_.consume(); }

  NativeWaitHandle nativeWaitHandle() const {
    return changed_.nativeWaitHandle();
  }

  void join() {
    std::thread worker;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (worker_.joinable()) worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
  }

 private:
  void updateProgress(float progress) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!activity_) return;
      activity_->progress =
          std::max(activity_->progress, std::clamp(progress, 0.0f, 1.0f));
    }
    changed_.signal();
  }

  void run(const Operation& operation) {
    TaskCompletion completion;
    try {
      completion = operation(
          [this](float progress) { updateProgress(progress); });
    } catch (const std::exception& exception) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (activity_) {
        completion.kind = activity_->kind;
        completion.sourceFile = activity_->sourceFile;
      }
      completion.outcome = TaskOutcome::Failed;
      completion.detail =
          std::string("Media processing failed: ") + exception.what();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (activity_) {
        completion.kind = activity_->kind;
        completion.sourceFile = activity_->sourceFile;
      }
      completion.outcome = TaskOutcome::Failed;
      completion.detail = "Media processing failed unexpectedly.";
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      completion_ = std::move(completion);
      activity_.reset();
    }
    changed_.signal();
  }

  void joinFinished() {
    std::thread finishedWorker;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!activity_ && worker_.joinable()) {
        finishedWorker = std::move(worker_);
      }
    }
    if (finishedWorker.joinable()) finishedWorker.join();
  }

  mutable std::mutex mutex_;
  std::thread worker_;
  WaitableSignal changed_;
  std::optional<TaskActivity> activity_;
  std::optional<TaskCompletion> completion_;
};

}  // namespace

struct Coordinator::Impl {
  explicit Impl(Backends backends)
      : analyzeMelody(std::move(backends.analyzeMelody)),
        splitLoop(std::move(backends.splitLoop)),
        subtitles(std::move(backends.subtitles)),
        audioSeparation(std::move(backends.audioSeparation)),
        audioSeparationAvailable(backends.audioSeparationAvailable) {}

  MelodyOperation analyzeMelody;
  LoopSplitOperation splitLoop;
  std::unique_ptr<playback_video_transcript::GenerationJob> subtitles;
  std::unique_ptr<audio_separation::Job> audioSeparation;
  bool audioSeparationAvailable = false;
  WorkerTask workerTask;
  bool subtitleCompletionPending = false;
  bool audioSeparationCompletionPending = false;
  std::optional<TaskCompletion> latestCompletion;

  bool completionPending() const {
    return workerTask.hasPendingCompletion() || subtitleCompletionPending ||
           audioSeparationCompletionPending;
  }
};

Coordinator::Coordinator(Operations operations)
    : Coordinator([operations = std::move(operations)]() mutable {
        Backends backends;
        backends.analyzeMelody = std::move(operations.analyzeMelody);
        backends.splitLoop = std::move(operations.splitLoop);
        backends.subtitles =
            std::make_unique<playback_video_transcript::GenerationJob>(
                std::move(operations.generateSubtitles));
        backends.audioSeparation = std::make_unique<audio_separation::Job>(
            std::move(operations.separateAudio));
        backends.audioSeparationAvailable =
            operations.audioSeparationAvailable;
        return backends;
      }()) {}

Coordinator::Coordinator(Backends backends)
    : impl_(std::make_unique<Impl>(std::move(backends))) {}

Coordinator::~Coordinator() { shutdown(); }

bool Coordinator::running() const {
  if (!impl_) return false;
  return impl_->workerTask.running() ||
         (impl_->subtitles && impl_->subtitles->snapshot().running()) ||
         (impl_->audioSeparation &&
          impl_->audioSeparation->snapshot().running());
}

std::optional<TaskActivity> Coordinator::activity() const {
  if (!impl_) return std::nullopt;
  if (std::optional<TaskActivity> activity = impl_->workerTask.activity()) {
    return activity;
  }
  if (impl_->subtitles) {
    const auto snapshot = impl_->subtitles->snapshot();
    if (snapshot.running()) {
      TaskActivity activity;
      activity.kind = TaskKind::SubtitleGeneration;
      activity.sourceFile = snapshot.sourceFile;
      activity.progress = std::clamp(snapshot.progress, 0.0f, 1.0f);
      activity.phase = snapshot.phase;
      activity.cancelling = snapshot.cancelling();
      activity.cancellable = true;
      return activity;
    }
  }
  if (impl_->audioSeparation) {
    const auto snapshot = impl_->audioSeparation->snapshot();
    if (snapshot.running()) {
      TaskActivity activity;
      activity.kind = TaskKind::AudioSeparation;
      activity.sourceFile = snapshot.sourceFile;
      activity.progress = std::clamp(snapshot.progress, 0.0f, 1.0f);
      activity.phase = snapshot.phase;
      activity.cancelling = snapshot.cancelling();
      activity.cancellable = true;
      return activity;
    }
  }
  return std::nullopt;
}

std::optional<TaskCompletion> Coordinator::latestCompletion() const {
  return impl_ ? impl_->latestCompletion : std::nullopt;
}

bool Coordinator::tryStartMelodyAnalysis(
    const std::filesystem::path& sourceFile, int trackIndex,
    const std::filesystem::path& outputFile) {
  if (!impl_ || !impl_->analyzeMelody || sourceFile.empty() ||
      outputFile.empty() || !isSupportedAudioExt(sourceFile) || running() ||
      impl_->completionPending()) {
    return false;
  }

  const MelodyOperation operation = impl_->analyzeMelody;
  TaskActivity activity;
  activity.kind = TaskKind::MelodyAnalysis;
  activity.sourceFile = sourceFile;
  const bool started = impl_->workerTask.tryStart(
      std::move(activity),
      [operation, sourceFile, trackIndex,
       outputFile](const WorkerTask::ProgressReporter& reportProgress) {
        std::string error;
        const bool succeeded = operation(sourceFile, trackIndex, outputFile,
                                         reportProgress, &error);
        TaskCompletion completion;
        completion.kind = TaskKind::MelodyAnalysis;
        completion.outcome = succeeded ? TaskOutcome::Succeeded
                                       : TaskOutcome::Failed;
        completion.sourceFile = sourceFile;
        completion.outputFile = outputFile;
        if (succeeded) {
          std::filesystem::path midiOutput = outputFile;
          midiOutput.replace_extension(".mid");
          completion.detail =
              "Saved " + toUtf8String(outputFile.filename()) + " and " +
              toUtf8String(midiOutput.filename());
        } else {
          completion.detail =
              error.empty() ? "Analysis failed." : std::move(error);
        }
        return completion;
      });
  if (started) impl_->latestCompletion.reset();
  return started;
}

bool Coordinator::tryStartLoopSplit(
    const std::filesystem::path& sourceFile,
    const std::filesystem::path& stingerOutput,
    const std::filesystem::path& loopOutput, const LoopSplitConfig& config) {
  if (!impl_ || !impl_->splitLoop || sourceFile.empty() ||
      stingerOutput.empty() || loopOutput.empty() ||
      !isSupportedAudioExt(sourceFile) || running() ||
      impl_->completionPending()) {
    return false;
  }

  const LoopSplitOperation operation = impl_->splitLoop;
  TaskActivity activity;
  activity.kind = TaskKind::LoopSplit;
  activity.sourceFile = sourceFile;
  const bool started = impl_->workerTask.tryStart(
      std::move(activity),
      [operation, sourceFile, stingerOutput, loopOutput,
       config](const WorkerTask::ProgressReporter&) {
        LoopSplitResult result;
        std::string error;
        const bool succeeded = operation(sourceFile, stingerOutput, loopOutput,
                                         config, &result, &error);
        TaskCompletion completion;
        completion.kind = TaskKind::LoopSplit;
        completion.outcome = succeeded ? TaskOutcome::Succeeded
                                       : TaskOutcome::Failed;
        completion.sourceFile = sourceFile;
        completion.outputFile = loopOutput;
        if (succeeded) {
          completion.detail =
              result.hasStinger
                  ? "Saved " + toUtf8String(stingerOutput.filename()) +
                        " and " + toUtf8String(loopOutput.filename())
                  : "Saved " + toUtf8String(loopOutput.filename()) +
                        " only";
        } else {
          completion.detail =
              error.empty() ? "Loop split failed." : std::move(error);
        }
        return completion;
      });
  if (started) impl_->latestCompletion.reset();
  return started;
}

playback_media_processing::SourceState Coordinator::sourceStateFor(
    const std::filesystem::path& sourceFile) const {
  playback_media_processing::SourceState state;
  const std::optional<TaskActivity> currentActivity = activity();
  state.backgroundTaskRunning = currentActivity.has_value();
  if (currentActivity &&
      samePath(currentActivity->sourceFile, sourceFile)) {
    state.subtitleGenerationRunning =
        currentActivity->kind == TaskKind::SubtitleGeneration;
    state.audioSeparationRunning =
        currentActivity->kind == TaskKind::AudioSeparation;
  }
  state.audioSeparationAvailable =
      audioSeparationAvailableFor(sourceFile);
  state.separatedAudioExists = hasSeparatedAudioFor(sourceFile);
  return state;
}

bool Coordinator::requestSubtitles(
    const std::filesystem::path& sourceFile) {
  if (!impl_ || !impl_->subtitles || !isSupportedVideoExt(sourceFile) ||
      running() || impl_->completionPending() ||
      !impl_->subtitles->tryStart(sourceFile)) {
    return false;
  }
  impl_->subtitleCompletionPending = true;
  impl_->latestCompletion.reset();
  return true;
}

bool Coordinator::requestAudioSeparation(
    const std::filesystem::path& sourceFile) {
  if (!audioSeparationAvailableFor(sourceFile) || running() ||
      impl_->completionPending() ||
      !impl_->audioSeparation->tryStart(sourceFile)) {
    return false;
  }
  impl_->audioSeparationCompletionPending = true;
  impl_->latestCompletion.reset();
  return true;
}

bool Coordinator::subtitleGenerationRunningFor(
    const std::filesystem::path& sourceFile) const {
  if (!impl_ || !impl_->subtitles) return false;
  const auto snapshot = impl_->subtitles->snapshot();
  return snapshot.running() && samePath(snapshot.sourceFile, sourceFile);
}

bool Coordinator::audioSeparationAvailableFor(
    const std::filesystem::path& sourceFile) const {
  return impl_ && impl_->audioSeparation &&
         impl_->audioSeparationAvailable &&
         (isSupportedVideoExt(sourceFile) ||
          isSupportedAudioExt(sourceFile)) &&
         !audio_separation::isManagedArtifactPath(sourceFile);
}

bool Coordinator::audioSeparationRunningFor(
    const std::filesystem::path& sourceFile) const {
  if (!impl_ || !impl_->audioSeparation) return false;
  const auto snapshot = impl_->audioSeparation->snapshot();
  return snapshot.running() && samePath(snapshot.sourceFile, sourceFile);
}

bool Coordinator::hasSeparatedAudioFor(
    const std::filesystem::path& sourceFile) const {
  return audio_separation::artifactsExistFor(sourceFile);
}

bool Coordinator::requestSubtitleCancellation() {
  return impl_ && impl_->subtitles && impl_->subtitles->requestCancel();
}

bool Coordinator::requestAudioSeparationCancellation() {
  return impl_ && impl_->audioSeparation &&
         impl_->audioSeparation->requestCancel();
}

bool Coordinator::cancelActive() {
  if (!impl_) return false;
  if (impl_->subtitles && impl_->subtitles->snapshot().running()) {
    return requestSubtitleCancellation();
  }
  if (impl_->audioSeparation &&
      impl_->audioSeparation->snapshot().running()) {
    return requestAudioSeparationCancellation();
  }
  return false;
}

PollResult Coordinator::poll() {
  PollResult result;
  if (!impl_) return result;

  result.changed = impl_->workerTask.consumeChanged();
  if (std::optional<TaskCompletion> completion =
          impl_->workerTask.takeCompletion()) {
    result.changed = true;
    impl_->latestCompletion = *completion;
    result.completions.push_back(std::move(*completion));
  }

  if (impl_->subtitles) {
    result.changed = impl_->subtitles->consumeChanged() || result.changed;
    if (auto completion = impl_->subtitles->takeCompletion()) {
      TaskCompletion taskCompletion;
      taskCompletion.kind = TaskKind::SubtitleGeneration;
      taskCompletion.outcome = outcomeFor(*completion);
      taskCompletion.sourceFile = completion->sourceFile;
      taskCompletion.outputFile = completion->outputFile;
      taskCompletion.detail = completion->error;
      impl_->subtitleCompletionPending = false;
      impl_->latestCompletion = taskCompletion;
      result.completions.push_back(std::move(taskCompletion));
      result.changed = true;
    }
  }

  if (impl_->audioSeparation) {
    result.changed =
        impl_->audioSeparation->consumeChanged() || result.changed;
    if (auto completion = impl_->audioSeparation->takeCompletion()) {
      TaskCompletion taskCompletion;
      taskCompletion.kind = TaskKind::AudioSeparation;
      taskCompletion.outcome = outcomeFor(*completion);
      taskCompletion.sourceFile = completion->sourceFile;
      taskCompletion.outputFile = completion->outputFiles.front();
      taskCompletion.detail = completion->error;
      impl_->audioSeparationCompletionPending = false;
      impl_->latestCompletion = taskCompletion;
      result.completions.push_back(std::move(taskCompletion));
      result.changed = true;
    }
  }
  return result;
}

std::vector<NativeWaitHandle> Coordinator::waitHandles() const {
  std::vector<NativeWaitHandle> handles;
  if (!impl_) return handles;
  const auto append = [&handles](NativeWaitHandle handle) {
    if (handle) handles.push_back(handle);
  };
  append(impl_->workerTask.nativeWaitHandle());
  if (impl_->subtitles) append(impl_->subtitles->nativeWaitHandle());
  if (impl_->audioSeparation) {
    append(impl_->audioSeparation->nativeWaitHandle());
  }
  return handles;
}

void Coordinator::shutdown() {
  if (!impl_) return;
  impl_->workerTask.join();
  if (impl_->subtitles) impl_->subtitles->cancelAndJoin();
  if (impl_->audioSeparation) impl_->audioSeparation->cancelAndJoin();
}

}  // namespace media_processing
