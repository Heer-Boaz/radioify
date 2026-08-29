#include "app/media_processing_coordinator.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include "audio/media_formats.h"
#include "audio/separation/artifact.h"
#include "core/file_output.h"
#include "core/path_identity.h"
#include "core/runtime_helpers.h"
#include "core/wake_event.h"
#include "playback/video/transcript/artifact.h"

namespace media_processing {

struct InteractivePlaybackState {
  std::mutex mutex;
  std::size_t leaseCount = 0;
  audio_separation::Job* audioSeparation = nullptr;
};

namespace {

using playback_media_processing::RequestFailure;
using playback_media_processing::RequestResult;

RequestResult rejected(RequestFailure failure, std::string detail = {}) {
  return RequestResult::rejected(failure, std::move(detail));
}

RequestResult rejected(
    const playback_media_processing::RequestError& error) {
  return RequestResult::rejected(
      error.failure, error.detail, error.blockingOperation,
      error.blockingSourceFile);
}

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

bool supportsAudioExport(const std::filesystem::path& sourceFile) {
  return isSupportedVideoExt(sourceFile) || isMiniaudioExt(sourceFile) ||
         isFfmpegAudioExt(sourceFile) || isM4aExt(sourceFile);
}

playback_media_processing::Outcome playbackOutcomeFor(TaskOutcome outcome) {
  switch (outcome) {
    case TaskOutcome::Succeeded:
      return playback_media_processing::Outcome::Succeeded;
    case TaskOutcome::Failed:
      return playback_media_processing::Outcome::Failed;
    case TaskOutcome::Cancelled:
      return playback_media_processing::Outcome::Cancelled;
  }
  return playback_media_processing::Outcome::Failed;
}

class WorkerTask {
 public:
  using ProgressReporter = Coordinator::ProgressReporter;
  using CancellationRequested = Coordinator::CancellationRequested;
  using CommitStarted = std::function<bool(std::string)>;
  using Operation = std::function<TaskCompletion(
      const ProgressReporter&, const CancellationRequested&,
      const CommitStarted&)>;

  explicit WorkerTask(WakeNotifier ownerWake = {})
      : ownerWake_(std::move(ownerWake)) {}

  ~WorkerTask() { cancelAndJoin(); }

  bool tryStart(TaskActivity activity, Operation operation) {
    joinFinished();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!operation || activity_ || completion_) return false;
      cancelRequested_.store(false, std::memory_order_relaxed);
      commitStarted_ = false;
      if (activity.progress) {
        activity.progress = std::clamp(*activity.progress, 0.0f, 1.0f);
      }
      activity_ = std::move(activity);
      try {
        worker_ = std::thread([this, operation = std::move(operation)]() {
          run(operation);
        });
      } catch (const std::exception& exception) {
        TaskCompletion completion;
        completion.id = activity_->id;
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
        completion.id = activity_->id;
        completion.kind = activity_->kind;
        completion.outcome = TaskOutcome::Failed;
        completion.sourceFile = activity_->sourceFile;
        completion.detail = "Could not start media processing.";
        completion_ = std::move(completion);
        activity_.reset();
      }
    }
    notifyChanged();
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

  bool consumeChanged() {
    return changed_.exchange(false, std::memory_order_acq_rel);
  }

  bool requestCancel() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!activity_ || !activity_->cancellable || activity_->cancelling) {
        return false;
      }
      activity_->cancelling = true;
      activity_->cancellable = false;
      cancelRequested_.store(true, std::memory_order_release);
    }
    notifyChanged();
    return true;
  }

  void cancelAndJoin() {
    requestCancel();
    std::thread worker;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (worker_.joinable()) worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
  }

 private:
  void notifyChanged() {
    changed_.store(true, std::memory_order_release);
    ownerWake_.notify();
  }

  void updateProgress(float progress, std::string phase) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!activity_) return;
      const float clamped = std::clamp(progress, 0.0f, 1.0f);
      activity_->progress = activity_->progress
                                ? std::max(*activity_->progress, clamped)
                                : clamped;
      if (!activity_->cancelling && !phase.empty()) {
        activity_->phase = std::move(phase);
      }
    }
    notifyChanged();
  }

  bool beginCommit(std::string phase) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!activity_ || activity_->cancelling ||
          cancelRequested_.load(std::memory_order_acquire)) {
        return false;
      }
      activity_->cancellable = false;
      commitStarted_ = true;
      if (!phase.empty()) activity_->phase = std::move(phase);
    }
    notifyChanged();
    return true;
  }

  void run(const Operation& operation) {
    TaskCompletion completion;
    try {
      completion = operation(
          [this](float progress, std::string phase) {
            updateProgress(progress, std::move(phase));
          },
          [this]() {
            return cancelRequested_.load(std::memory_order_acquire);
          },
          [this](std::string phase) {
            return beginCommit(std::move(phase));
          });
    } catch (const std::exception& exception) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (activity_) {
        completion.id = activity_->id;
        completion.kind = activity_->kind;
        completion.sourceFile = activity_->sourceFile;
      }
      completion.outcome = TaskOutcome::Failed;
      completion.detail =
          std::string("Media processing failed: ") + exception.what();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (activity_) {
        completion.id = activity_->id;
        completion.kind = activity_->kind;
        completion.sourceFile = activity_->sourceFile;
      }
      completion.outcome = TaskOutcome::Failed;
      completion.detail = "Media processing failed unexpectedly.";
    }

    bool commitWasStarted = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      commitWasStarted = commitStarted_;
    }
    if (completion.succeeded() && !commitWasStarted) {
      completion.outcome = TaskOutcome::Failed;
      completion.detail =
          "The media backend completed without publishing its output "
          "through the commit barrier.";
    }

    if (cancelRequested_.load(std::memory_order_acquire) &&
        !completion.succeeded()) {
      completion.outcome = TaskOutcome::Cancelled;
      completion.detail.clear();
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (activity_) {
        completion.id = activity_->id;
      }
      completion_ = std::move(completion);
      activity_.reset();
    }
    notifyChanged();
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
  std::atomic<bool> changed_{false};
  std::atomic<bool> cancelRequested_{false};
  bool commitStarted_ = false;
  WakeNotifier ownerWake_;
  std::optional<TaskActivity> activity_;
  std::optional<TaskCompletion> completion_;
};

}  // namespace

std::optional<playback_media_processing::Completion> completionForPlayback(
    const TaskCompletion& completion) {
  playback_media_processing::Completion projected;
  projected.taskId = completion.id;
  switch (completion.kind) {
    case TaskKind::MelodyAnalysis:
    case TaskKind::LoopSplit:
      return std::nullopt;
    case TaskKind::SubtitleGeneration:
    case TaskKind::AudioSeparation:
    case TaskKind::AudioExport:
    case TaskKind::TranscriptTextExport:
      projected.operation = completion.kind;
      break;
  }
  projected.outcome = playbackOutcomeFor(completion.outcome);
  projected.sourceFile = completion.sourceFile;
  projected.outputFile = completion.outputFile;
  projected.diagnosticLog = completion.diagnosticLog;
  projected.detail = completion.detail;
  return projected;
}

struct Coordinator::Impl {
  explicit Impl(Backends backends)
      : wakeEvent(),
        workerTask(wakeEvent.notifier()),
        analyzeMelody(std::move(backends.analyzeMelody)),
        splitLoop(std::move(backends.splitLoop)),
        exportAudio(std::move(backends.exportAudio)),
        exportTranscriptText(std::move(backends.exportTranscriptText)),
        subtitles(
            std::make_unique<playback_video_transcript::GenerationJob>(
                std::move(backends.generateSubtitles), wakeEvent.notifier())),
        audioSeparation(std::make_unique<audio_separation::Job>(
            std::move(backends.separateAudio), wakeEvent.notifier())),
        interactivePlayback(std::make_shared<InteractivePlaybackState>()) {
    interactivePlayback->audioSeparation = audioSeparation.get();
  }

  WakeEvent wakeEvent;
  WorkerTask workerTask;
  MelodyOperation analyzeMelody;
  LoopSplitOperation splitLoop;
  FileExportOperation exportAudio;
  FileExportOperation exportTranscriptText;
  std::unique_ptr<playback_video_transcript::GenerationJob> subtitles;
  std::unique_ptr<audio_separation::Job> audioSeparation;
  std::shared_ptr<InteractivePlaybackState> interactivePlayback;
  std::optional<TaskId> subtitleTask;
  std::optional<TaskId> audioSeparationTask;
  std::vector<TaskCompletion> queuedCompletions;
  std::optional<TaskCompletion> latestCompletion;
  std::uint64_t nextTaskId = 1;

  TaskId allocateTaskId() { return TaskId{nextTaskId++}; }

  bool completionPending() const {
    return workerTask.hasPendingCompletion() || subtitleTask.has_value() ||
           audioSeparationTask.has_value();
  }
};

Coordinator::Coordinator(Operations operations)
    : Coordinator([operations = std::move(operations)]() mutable {
        Backends backends;
        backends.analyzeMelody = std::move(operations.analyzeMelody);
        backends.splitLoop = std::move(operations.splitLoop);
        backends.generateSubtitles =
            std::move(operations.generateSubtitles);
        backends.separateAudio = std::move(operations.separateAudio);
        backends.exportAudio = std::move(operations.exportAudio);
        backends.exportTranscriptText =
            std::move(operations.exportTranscriptText);
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
      activity.id = impl_->subtitleTask.value_or(TaskId{});
      activity.kind = TaskKind::SubtitleGeneration;
      activity.sourceFile = snapshot.sourceFile;
      activity.progress = std::clamp(snapshot.progress, 0.0f, 1.0f);
      activity.phase = snapshot.phase;
      activity.cancelling = snapshot.cancelling();
      activity.cancellable = snapshot.cancellable();
      return activity;
    }
  }
  if (impl_->audioSeparation) {
    const auto snapshot = impl_->audioSeparation->snapshot();
    if (snapshot.running()) {
      TaskActivity activity;
      activity.id = impl_->audioSeparationTask.value_or(TaskId{});
      activity.kind = TaskKind::AudioSeparation;
      activity.sourceFile = snapshot.sourceFile;
      activity.progress = std::clamp(snapshot.progress, 0.0f, 1.0f);
      activity.phase = snapshot.phase;
      activity.cancelling = snapshot.cancelling();
      activity.cancellable = snapshot.cancellable();
      switch (snapshot.scheduling) {
        case audio_separation::JobSchedulingState::Running:
          break;
        case audio_separation::JobSchedulingState::Suspending:
          activity.scheduling = TaskSchedulingState::Suspending;
          activity.phase = "Yielding the GPU to video playback";
          break;
        case audio_separation::JobSchedulingState::Suspended:
          activity.scheduling = TaskSchedulingState::Suspended;
          activity.phase = "Video playback has priority";
          break;
      }
      return activity;
    }
  }
  return std::nullopt;
}

std::optional<TaskCompletion> Coordinator::latestCompletion() const {
  return impl_ ? impl_->latestCompletion : std::nullopt;
}

bool Coordinator::collectReadyCompletions() {
  if (!impl_) return false;
  bool collected = false;

  if (std::optional<TaskCompletion> completion =
          impl_->workerTask.takeCompletion()) {
    impl_->latestCompletion = *completion;
    impl_->queuedCompletions.push_back(std::move(*completion));
    collected = true;
  }

  if (impl_->subtitles) {
    if (auto completion = impl_->subtitles->takeCompletion()) {
      TaskCompletion taskCompletion;
      taskCompletion.id = impl_->subtitleTask.value_or(TaskId{});
      taskCompletion.kind = TaskKind::SubtitleGeneration;
      taskCompletion.outcome = outcomeFor(*completion);
      taskCompletion.sourceFile = completion->sourceFile;
      taskCompletion.outputFile = completion->outputFile;
      taskCompletion.detail = completion->error;
      impl_->subtitleTask.reset();
      impl_->latestCompletion = taskCompletion;
      impl_->queuedCompletions.push_back(std::move(taskCompletion));
      collected = true;
    }
  }

  if (impl_->audioSeparation) {
    if (auto completion = impl_->audioSeparation->takeCompletion()) {
      TaskCompletion taskCompletion;
      taskCompletion.id =
          impl_->audioSeparationTask.value_or(TaskId{});
      taskCompletion.kind = TaskKind::AudioSeparation;
      taskCompletion.outcome = outcomeFor(*completion);
      taskCompletion.sourceFile = completion->sourceFile;
      taskCompletion.outputFile = completion->outputFiles.front();
      taskCompletion.diagnosticLog = completion->diagnosticLog;
      taskCompletion.detail = completion->error;
      impl_->audioSeparationTask.reset();
      impl_->latestCompletion = taskCompletion;
      impl_->queuedCompletions.push_back(std::move(taskCompletion));
      collected = true;
    }
  }
  return collected;
}

std::optional<playback_media_processing::RequestError>
Coordinator::startConflict() {
  if (!impl_) return std::nullopt;
  collectReadyCompletions();
  if (const std::optional<TaskActivity> current = activity()) {
    playback_media_processing::RequestError error;
    error.failure = RequestFailure::Busy;
    error.blockingOperation = current->kind;
    error.blockingSourceFile = current->sourceFile;
    return error;
  }
  if (impl_->completionPending()) {
    // A worker can finish between the first collection and activity snapshot.
    // Collect once more before exposing an owner-thread race to the user.
    collectReadyCompletions();
  }
  if (impl_->completionPending()) {
    playback_media_processing::RequestError error;
    error.failure = RequestFailure::CompletionPending;
    return error;
  }
  return std::nullopt;
}

RequestResult Coordinator::tryStartMelodyAnalysis(
    const std::filesystem::path& sourceFile, int trackIndex,
    const std::filesystem::path& outputFile) {
  if (!impl_ || !impl_->analyzeMelody) {
    return rejected(RequestFailure::BackendUnavailable,
                    "the melody-analysis backend is not configured");
  }
  if (sourceFile.empty()) return rejected(RequestFailure::InvalidSource);
  if (!isSupportedAudioExt(sourceFile)) {
    return rejected(RequestFailure::UnsupportedSource);
  }
  if (trackIndex < 0) return rejected(RequestFailure::InvalidSelection);
  if (outputFile.empty()) {
    return rejected(RequestFailure::InvalidDestination);
  }
  if (const auto conflict = startConflict()) return rejected(*conflict);

  const MelodyOperation operation = impl_->analyzeMelody;
  TaskActivity activity;
  activity.id = impl_->allocateTaskId();
  activity.kind = TaskKind::MelodyAnalysis;
  activity.sourceFile = sourceFile;
  activity.progress = 0.0f;
  activity.phase = "Preparing melody analysis";
  activity.cancellable = true;
  const bool started = impl_->workerTask.tryStart(
      std::move(activity),
      [operation, sourceFile, trackIndex,
       outputFile](const WorkerTask::ProgressReporter& reportProgress,
                   const WorkerTask::CancellationRequested&
                       cancellationRequested,
                   const WorkerTask::CommitStarted& beginCommit) {
        std::string error;
        const bool succeeded = operation(sourceFile, trackIndex, outputFile,
                                         reportProgress,
                                         cancellationRequested,
                                         [&]() {
                                           return beginCommit(
                                               "Publishing melody files");
                                         },
                                         &error);
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
  if (!started) {
    return rejected(RequestFailure::InternalError,
                    "the melody-analysis worker rejected the request");
  }
  impl_->latestCompletion.reset();
  return RequestResult::accepted();
}

RequestResult Coordinator::tryStartLoopSplit(
    const std::filesystem::path& sourceFile,
    const std::filesystem::path& stingerOutput,
    const std::filesystem::path& loopOutput, const LoopSplitConfig& config) {
  if (!impl_ || !impl_->splitLoop) {
    return rejected(RequestFailure::BackendUnavailable,
                    "the loop-splitting backend is not configured");
  }
  if (sourceFile.empty()) return rejected(RequestFailure::InvalidSource);
  if (!isSupportedAudioExt(sourceFile)) {
    return rejected(RequestFailure::UnsupportedSource);
  }
  if (config.trackIndex < 0) {
    return rejected(RequestFailure::InvalidSelection);
  }
  if (stingerOutput.empty() || loopOutput.empty()) {
    return rejected(RequestFailure::InvalidDestination);
  }
  if (const auto conflict = startConflict()) return rejected(*conflict);

  const LoopSplitOperation operation = impl_->splitLoop;
  TaskActivity activity;
  activity.id = impl_->allocateTaskId();
  activity.kind = TaskKind::LoopSplit;
  activity.sourceFile = sourceFile;
  activity.phase = "Preparing loop split";
  activity.cancellable = true;
  const bool started = impl_->workerTask.tryStart(
      std::move(activity),
      [operation, sourceFile, stingerOutput, loopOutput,
       config](const WorkerTask::ProgressReporter& reportProgress,
               const WorkerTask::CancellationRequested&
                   cancellationRequested,
               const WorkerTask::CommitStarted& beginCommit) {
        LoopSplitResult result;
        std::string error;
        const bool succeeded = operation(sourceFile, stingerOutput, loopOutput,
                                         config, &result, reportProgress,
                                         cancellationRequested,
                                         [&]() {
                                           return beginCommit(
                                               "Publishing loop files");
                                         },
                                         &error);
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
  if (!started) {
    return rejected(RequestFailure::InternalError,
                    "the loop-splitting worker rejected the request");
  }
  impl_->latestCompletion.reset();
  return RequestResult::accepted();
}

RequestResult Coordinator::tryStartFileExport(
    TaskKind kind, const std::filesystem::path& sourceFile,
    const std::filesystem::path& outputFile,
    const FileExportOperation& operation, const char* initialPhase,
    const char* fallbackError) {
  if (!impl_ || !operation) {
    return rejected(RequestFailure::BackendUnavailable);
  }
  if (sourceFile.empty()) return rejected(RequestFailure::InvalidSource);
  if (outputFile.empty()) {
    return rejected(RequestFailure::InvalidDestination);
  }
  if (const auto conflict = startConflict()) return rejected(*conflict);
  TaskActivity activity;
  activity.id = impl_->allocateTaskId();
  activity.kind = kind;
  activity.sourceFile = sourceFile;
  activity.progress = 0.0f;
  activity.phase = initialPhase;
  activity.cancellable = true;
  const bool started = impl_->workerTask.tryStart(
      std::move(activity),
      [kind, operation, sourceFile, outputFile,
       fallbackError](const WorkerTask::ProgressReporter& reportProgress,
                      const WorkerTask::CancellationRequested&
                          cancellationRequested,
                      const WorkerTask::CommitStarted& beginCommit) {
        std::string error;
        const bool succeeded = operation(sourceFile, outputFile,
                                         reportProgress,
                                         cancellationRequested,
                                         [&]() {
                                           return beginCommit(
                                               "Publishing output");
                                         },
                                         &error);
        TaskCompletion completion;
        completion.kind = kind;
        completion.outcome = succeeded ? TaskOutcome::Succeeded
                                       : TaskOutcome::Failed;
        completion.sourceFile = sourceFile;
        completion.outputFile = outputFile;
        if (!succeeded) {
          completion.detail =
              error.empty() ? fallbackError : std::move(error);
        }
        return completion;
      });
  if (!started) {
    return rejected(RequestFailure::InternalError,
                    "the export worker rejected the request");
  }
  impl_->latestCompletion.reset();
  return RequestResult::accepted();
}

playback_media_processing::SourceState Coordinator::sourceStateFor(
    const std::filesystem::path& sourceFile) const {
  playback_media_processing::SourceState state;
  const std::optional<TaskActivity> currentActivity = activity();
  state.backgroundTaskRunning = currentActivity.has_value();
  state.subtitleGenerationAvailable =
      impl_ && impl_->subtitles && impl_->subtitles->configured() &&
      isSupportedVideoExt(sourceFile);
  state.hasGeneratedSubtitles =
      !playback_video_transcript::activeTranscriptPathForVideo(sourceFile)
           .empty();
  if (currentActivity &&
      samePath(currentActivity->sourceFile, sourceFile)) {
    state.activeTaskId = currentActivity->id;
    state.activeTaskCancellable = currentActivity->cancellable;
    state.subtitleGenerationRunning =
        currentActivity->kind == TaskKind::SubtitleGeneration;
    state.audioSeparationRunning =
        currentActivity->kind == TaskKind::AudioSeparation;
    state.audioExportRunning =
        currentActivity->kind == TaskKind::AudioExport;
    state.transcriptTextExportRunning =
        currentActivity->kind == TaskKind::TranscriptTextExport;
  }
  state.audioSeparationAvailable =
      audioSeparationAvailableFor(sourceFile);
  state.separatedAudioExists = hasSeparatedAudioFor(sourceFile);
  state.audioExportAvailable = audioExportAvailableFor(sourceFile);
  state.transcriptTextExportAvailable =
      transcriptTextExportAvailableFor(sourceFile);
  return state;
}

RequestResult Coordinator::requestSubtitles(
    const std::filesystem::path& sourceFile) {
  if (!impl_ || !impl_->subtitles || !impl_->subtitles->configured()) {
    return rejected(RequestFailure::BackendUnavailable,
                    "the Vulkan subtitle-generation backend is not "
                    "configured");
  }
  if (sourceFile.empty()) return rejected(RequestFailure::InvalidSource);
  if (!isSupportedVideoExt(sourceFile)) {
    return rejected(RequestFailure::UnsupportedSource);
  }
  if (const auto conflict = startConflict()) return rejected(*conflict);
  const TaskId task = impl_->allocateTaskId();
  impl_->subtitleTask = task;
  if (!impl_->subtitles->tryStart(sourceFile)) {
    impl_->subtitleTask.reset();
    return rejected(RequestFailure::InternalError,
                    "the subtitle-generation worker rejected the request");
  }
  impl_->latestCompletion.reset();
  return RequestResult::accepted();
}

RequestResult Coordinator::requestAudioSeparation(
    const std::filesystem::path& sourceFile) {
  if (!impl_ || !impl_->audioSeparation ||
      !impl_->audioSeparation->configured()) {
    return rejected(RequestFailure::BackendUnavailable,
                    "the native NVIDIA audio-separation backend is not "
                    "configured");
  }
  if (sourceFile.empty()) return rejected(RequestFailure::InvalidSource);
  if (!isSupportedVideoExt(sourceFile) &&
      !isSupportedAudioExt(sourceFile)) {
    return rejected(RequestFailure::UnsupportedSource);
  }
  if (audio_separation::isManagedArtifactPath(sourceFile)) {
    return rejected(RequestFailure::ManagedArtifact);
  }
  if (const auto conflict = startConflict()) return rejected(*conflict);
  bool initiallyPaused = false;
  {
    std::lock_guard<std::mutex> lock(impl_->interactivePlayback->mutex);
    initiallyPaused = impl_->interactivePlayback->leaseCount > 0;
  }
  const TaskId task = impl_->allocateTaskId();
  impl_->audioSeparationTask = task;
  if (!impl_->audioSeparation->tryStart(
          sourceFile,
          audio_separation::JobStartOptions{initiallyPaused})) {
    impl_->audioSeparationTask.reset();
    return rejected(RequestFailure::InternalError,
                    "the audio-separation worker rejected the request");
  }
  impl_->latestCompletion.reset();
  return RequestResult::accepted();
}

RequestResult Coordinator::requestAudioExport(
    const std::filesystem::path& sourceFile) {
  if (!impl_ || !impl_->exportAudio) {
    return rejected(RequestFailure::BackendUnavailable,
                    "the lossless audio-export backend is not configured");
  }
  if (sourceFile.empty()) return rejected(RequestFailure::InvalidSource);
  if (!supportsAudioExport(sourceFile)) {
    return rejected(RequestFailure::UnsupportedSource);
  }
  if (audio_separation::isManagedArtifactPath(sourceFile)) {
    return rejected(RequestFailure::ManagedArtifact);
  }
  const std::filesystem::path outputFile = file_output::uniqueSiblingPath(
      sourceFile, L" - audio", L".flac");
  return tryStartFileExport(TaskKind::AudioExport, sourceFile, outputFile,
                            impl_->exportAudio, "Preparing audio export",
                            "Audio export failed.");
}

RequestResult Coordinator::requestTranscriptTextExport(
    const std::filesystem::path& sourceFile) {
  if (!impl_ || !impl_->exportTranscriptText) {
    return rejected(RequestFailure::BackendUnavailable,
                    "the transcript-export backend is not configured");
  }
  if (sourceFile.empty()) return rejected(RequestFailure::InvalidSource);
  if (!isSupportedVideoExt(sourceFile)) {
    return rejected(RequestFailure::UnsupportedSource);
  }
  if (playback_video_transcript::activeTranscriptPathForVideo(sourceFile)
          .empty()) {
    return rejected(RequestFailure::MissingTranscript);
  }
  const std::filesystem::path outputFile = file_output::uniqueSiblingPath(
      sourceFile, L" - transcript", L".txt");
  return tryStartFileExport(
      TaskKind::TranscriptTextExport, sourceFile, outputFile,
      impl_->exportTranscriptText, "Preparing transcript export",
      "Transcript export failed.");
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
         impl_->audioSeparation->configured() &&
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

bool Coordinator::audioExportAvailableFor(
    const std::filesystem::path& sourceFile) const {
  return impl_ && impl_->exportAudio && supportsAudioExport(sourceFile) &&
         !audio_separation::isManagedArtifactPath(sourceFile);
}

bool Coordinator::transcriptTextExportAvailableFor(
    const std::filesystem::path& sourceFile) const {
  return impl_ && impl_->exportTranscriptText &&
         isSupportedVideoExt(sourceFile) &&
         !playback_video_transcript::activeTranscriptPathForVideo(sourceFile)
              .empty();
}

RequestResult Coordinator::requestSubtitleCancellation() {
  if (!impl_ || !impl_->subtitles || !impl_->subtitles->configured()) {
    return rejected(RequestFailure::BackendUnavailable);
  }
  const auto snapshot = impl_->subtitles->snapshot();
  if (snapshot.cancelling()) {
    return rejected(RequestFailure::AlreadyCancelling);
  }
  if (!snapshot.running()) return rejected(RequestFailure::NotRunning);
  return impl_->subtitles->requestCancel()
             ? RequestResult::accepted()
             : rejected(RequestFailure::InternalError,
                        "the subtitle-generation worker rejected "
                        "cancellation");
}

RequestResult Coordinator::requestAudioSeparationCancellation() {
  if (!impl_ || !impl_->audioSeparation ||
      !impl_->audioSeparation->configured()) {
    return rejected(RequestFailure::BackendUnavailable);
  }
  const auto snapshot = impl_->audioSeparation->snapshot();
  if (snapshot.cancelling()) {
    return rejected(RequestFailure::AlreadyCancelling);
  }
  if (!snapshot.running()) return rejected(RequestFailure::NotRunning);
  return impl_->audioSeparation->requestCancel()
             ? RequestResult::accepted()
             : rejected(RequestFailure::InternalError,
                        "the audio-separation worker rejected cancellation");
}

RequestResult Coordinator::requestMediaExportCancellation() {
  if (!impl_) return rejected(RequestFailure::BackendUnavailable);
  const std::optional<TaskActivity> current =
      impl_->workerTask.activity();
  if (!current ||
      (current->kind != TaskKind::AudioExport &&
       current->kind != TaskKind::TranscriptTextExport)) {
    return rejected(RequestFailure::NotRunning);
  }
  if (current->cancelling) {
    return rejected(RequestFailure::AlreadyCancelling);
  }
  return impl_->workerTask.requestCancel()
             ? RequestResult::accepted()
             : rejected(RequestFailure::InternalError,
                        "the export worker rejected cancellation");
}

bool Coordinator::cancelActive(TaskId expectedTask) {
  if (!impl_) return false;
  const std::optional<TaskActivity> current = activity();
  if (!current || current->id != expectedTask) {
    return false;
  }
  switch (current->kind) {
    case TaskKind::MelodyAnalysis:
    case TaskKind::LoopSplit:
    case TaskKind::AudioExport:
    case TaskKind::TranscriptTextExport:
      return impl_->workerTask.requestCancel();
    case TaskKind::SubtitleGeneration:
      return requestSubtitleCancellation().wasAccepted();
    case TaskKind::AudioSeparation:
      return requestAudioSeparationCancellation().wasAccepted();
  }
  return false;
}

Coordinator::InteractivePlaybackLease
Coordinator::acquireInteractivePlayback() {
  if (!impl_) return {};
  const std::shared_ptr<InteractivePlaybackState> state =
      impl_->interactivePlayback;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->leaseCount;
    if (state->leaseCount == 1 && state->audioSeparation) {
      state->audioSeparation->setPaused(true);
    }
  }
  return InteractivePlaybackLease(state);
}

PollResult Coordinator::poll() {
  PollResult result;
  if (!impl_) return result;

  result.changed = impl_->wakeEvent.consume();
  result.changed = impl_->workerTask.consumeChanged() || result.changed;
  if (impl_->subtitles) {
    result.changed = impl_->subtitles->consumeChanged() || result.changed;
  }
  if (impl_->audioSeparation) {
    result.changed =
        impl_->audioSeparation->consumeChanged() || result.changed;
  }

  result.changed = collectReadyCompletions() || result.changed;
  if (!impl_->queuedCompletions.empty()) {
    result.changed = true;
    result.completions.swap(impl_->queuedCompletions);
  }
  return result;
}

NativeWaitHandle Coordinator::waitHandle() const {
  return impl_ ? impl_->wakeEvent.nativeWaitHandle() : NativeWaitHandle{};
}

void Coordinator::shutdown() {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lock(impl_->interactivePlayback->mutex);
    impl_->interactivePlayback->audioSeparation = nullptr;
  }
  impl_->workerTask.cancelAndJoin();
  if (impl_->subtitles) impl_->subtitles->cancelAndJoin();
  if (impl_->audioSeparation) impl_->audioSeparation->cancelAndJoin();
}

Coordinator::InteractivePlaybackLease::~InteractivePlaybackLease() {
  reset();
}

Coordinator::InteractivePlaybackLease::InteractivePlaybackLease(
    InteractivePlaybackLease&& other) noexcept
    : state_(std::move(other.state_)) {}

Coordinator::InteractivePlaybackLease&
Coordinator::InteractivePlaybackLease::operator=(
    InteractivePlaybackLease&& other) noexcept {
  if (this == &other) return *this;
  reset();
  state_ = std::move(other.state_);
  return *this;
}

bool Coordinator::InteractivePlaybackLease::ready() const {
  if (!state_) return true;
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!state_->audioSeparation) return true;
  const audio_separation::JobSnapshot snapshot =
      state_->audioSeparation->snapshot();
  return !snapshot.running() ||
         snapshot.scheduling ==
             audio_separation::JobSchedulingState::Suspended;
}

void Coordinator::InteractivePlaybackLease::reset() {
  std::shared_ptr<InteractivePlaybackState> state = std::move(state_);
  if (!state) return;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->leaseCount == 0) return;
  --state->leaseCount;
  if (state->leaseCount == 0 && state->audioSeparation) {
    state->audioSeparation->setPaused(false);
  }
}

}  // namespace media_processing
