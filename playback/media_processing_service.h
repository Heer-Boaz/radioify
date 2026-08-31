#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "playback/audio_separation_availability.h"
#include "playback/media_processing_task_id.h"

namespace playback_media_processing {

enum class Operation {
  MelodyAnalysis,
  LoopSplit,
  SubtitleGeneration,
  AudioSeparationSetup,
  AudioSeparation,
  AudioExport,
  TranscriptTextExport,
};

enum class RequestFailure {
  InvalidSource,
  UnsupportedSource,
  InvalidSelection,
  InvalidDestination,
  BackendUnavailable,
  ManagedArtifact,
  MissingTranscript,
  Busy,
  CompletionPending,
  NotRunning,
  AlreadyCancelling,
  InternalError,
};

struct RequestError {
  RequestFailure failure = RequestFailure::InternalError;
  std::optional<Operation> blockingOperation;
  std::filesystem::path blockingSourceFile;
  std::string detail;
};

// Value result for a user request. Rejections retain machine-readable context
// until the presentation boundary instead of collapsing every cause to false.
class [[nodiscard]] RequestResult {
 public:
  static RequestResult accepted() { return RequestResult(std::nullopt); }

  static RequestResult rejected(RequestFailure failure,
                                std::string detail = {},
                                std::optional<Operation> blockingOperation =
                                    std::nullopt,
                                std::filesystem::path blockingSourceFile = {}) {
    RequestError error;
    error.failure = failure;
    error.blockingOperation = blockingOperation;
    error.blockingSourceFile = std::move(blockingSourceFile);
    error.detail = std::move(detail);
    return RequestResult(std::move(error));
  }

  [[nodiscard]] bool wasAccepted() const { return !error_; }
  const std::optional<RequestError>& error() const { return error_; }

 private:
  explicit RequestResult(std::optional<RequestError> error)
      : error_(std::move(error)) {}

  std::optional<RequestError> error_;
};

enum class Outcome {
  Succeeded,
  Failed,
  Cancelled,
};

struct Completion {
  TaskId taskId;
  Operation operation = Operation::SubtitleGeneration;
  Outcome outcome = Outcome::Failed;
  std::filesystem::path sourceFile;
  std::filesystem::path outputFile;
  std::filesystem::path diagnosticLog;
  std::string diagnosticDetail;
  std::string detail;

  bool succeeded() const { return outcome == Outcome::Succeeded; }
};

enum class SchedulingState {
  Running,
  Suspending,
  Suspended,
};

// Read-only application task state projected into playback surfaces. This is
// deliberately a value protocol: playback never receives a worker, callback,
// or application-coordinator pointer.
struct Activity {
  TaskId taskId;
  Operation operation = Operation::SubtitleGeneration;
  std::filesystem::path sourceFile;
  std::string processingEngine;
  std::optional<float> progress;
  std::string phase;
  bool cancelling = false;
  bool cancellable = false;
  SchedulingState scheduling = SchedulingState::Running;

  friend bool operator==(const Activity& left, const Activity& right) {
    return left.taskId == right.taskId &&
           left.operation == right.operation &&
           left.sourceFile == right.sourceFile &&
           left.processingEngine == right.processingEngine &&
           left.progress == right.progress && left.phase == right.phase &&
           left.cancelling == right.cancelling &&
           left.cancellable == right.cancellable &&
           left.scheduling == right.scheduling;
  }
  friend bool operator!=(const Activity& left, const Activity& right) {
    return !(left == right);
  }
};

struct SourceState {
  bool backgroundTaskRunning = false;
  // Present only when this source owns the active application task. UI
  // confirmations must freeze this identity instead of resolving a later task
  // from operation and path alone.
  std::optional<TaskId> activeTaskId;
  bool activeTaskCancellable = false;
  bool subtitleGenerationAvailable = false;
  bool subtitleGenerationRunning = false;
  bool hasGeneratedSubtitles = false;
  AudioSeparationAvailability audioSeparationAvailability =
      AudioSeparationAvailability::Unavailable;
  bool audioSeparationSetupRunning = false;
  bool audioSeparationRunning = false;
  bool separatedAudioExists = false;
  bool audioExportAvailable = false;
  bool audioExportRunning = false;
  bool transcriptTextExportAvailable = false;
  bool transcriptTextExportRunning = false;
};

// Narrow application service consumed by playback surfaces. Implementations
// own task lifecycle and source-state synchronization; callers only express
// user intent.
class Service {
 public:
  virtual ~Service() = default;

  virtual SourceState sourceStateFor(
      const std::filesystem::path& sourceFile) const = 0;
  virtual RequestResult requestSubtitles(
      const std::filesystem::path& sourceFile) = 0;
  virtual RequestResult requestSubtitleCancellation() = 0;
  virtual RequestResult requestAudioExport(
      const std::filesystem::path& sourceFile) = 0;
  virtual RequestResult requestTranscriptTextExport(
      const std::filesystem::path& sourceFile) = 0;
  virtual RequestResult requestMediaExportCancellation() = 0;
  virtual RequestResult requestAudioSeparationSetup(
      const std::filesystem::path& sourceFile) = 0;
  virtual RequestResult requestAudioSeparationSetupCancellation() = 0;
  virtual RequestResult requestAudioSeparation(
      const std::filesystem::path& sourceFile) = 0;
  virtual RequestResult requestAudioSeparationCancellation() = 0;
  // Identity-bound cancellation used after a confirmation dialog. A stale
  // surface must never cancel whichever task happened to start next.
  virtual RequestResult requestTaskCancellation(TaskId expectedTask) = 0;
};

}  // namespace playback_media_processing
