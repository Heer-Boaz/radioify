#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio/loopsplit/loopsplit.h"
#include "audio/separation/job.h"
#include "app/media_processing_task_id.h"
#include "core/native_wait_handle.h"
#include "playback/media_processing_service.h"
#include "playback/video/transcript/generation_job.h"

class AudioPlaybackRuntime;

namespace media_processing {

using TaskKind = playback_media_processing::Operation;

enum class TaskOutcome {
  Succeeded,
  Failed,
  Cancelled,
};

enum class TaskSchedulingState {
  Running,
  Suspending,
  Suspended,
};

struct TaskActivity {
  TaskId id;
  TaskKind kind = TaskKind::MelodyAnalysis;
  std::filesystem::path sourceFile;
  // Empty means that the backend cannot measure completion yet. UI surfaces
  // must render an indeterminate state instead of inventing a percentage.
  std::optional<float> progress;
  std::string phase;
  bool cancelling = false;
  bool cancellable = false;
  TaskSchedulingState scheduling = TaskSchedulingState::Running;
};

struct TaskCompletion {
  TaskId id;
  TaskKind kind = TaskKind::MelodyAnalysis;
  TaskOutcome outcome = TaskOutcome::Failed;
  std::filesystem::path sourceFile;
  std::filesystem::path outputFile;
  std::filesystem::path diagnosticLog;
  std::string detail;

  bool succeeded() const { return outcome == TaskOutcome::Succeeded; }
};

struct PollResult {
  bool changed = false;
  std::vector<TaskCompletion> completions;
};

// Projects application-wide task results onto the narrower protocol consumed
// by an active playback session. Browser-only tasks intentionally have no
// playback completion.
std::optional<playback_media_processing::Completion> completionForPlayback(
    const TaskCompletion& completion);

struct InteractivePlaybackState;

// Application-level owner of Radioify's mutually-exclusive, offline media
// processing. UI surfaces observe one common activity/completion contract
// instead of understanding every worker's lifecycle and synchronization.
class Coordinator final : public playback_media_processing::Service {
 public:
  class InteractivePlaybackLease {
   public:
    InteractivePlaybackLease() = default;
    ~InteractivePlaybackLease();

    InteractivePlaybackLease(InteractivePlaybackLease&& other) noexcept;
    InteractivePlaybackLease& operator=(
        InteractivePlaybackLease&& other) noexcept;

    InteractivePlaybackLease(const InteractivePlaybackLease&) = delete;
    InteractivePlaybackLease& operator=(const InteractivePlaybackLease&) =
        delete;

    bool ready() const;
    void reset();

   private:
    friend class Coordinator;
    explicit InteractivePlaybackLease(
        std::shared_ptr<InteractivePlaybackState> state)
        : state_(std::move(state)) {}

    std::shared_ptr<InteractivePlaybackState> state_;
  };

  using ProgressReporter = std::function<void(float, std::string)>;
  using CancellationRequested = std::function<bool()>;
  // The backend must claim this barrier immediately before publishing staged
  // output. It returns false when cancellation won the race; once it returns
  // true, cancellation is no longer advertised or accepted.
  using CommitStarted = std::function<bool()>;
  using MelodyOperation = std::function<bool(
      const std::filesystem::path&, int, const std::filesystem::path&,
      const ProgressReporter&, const CancellationRequested&,
      const CommitStarted&, std::string*)>;
  using LoopSplitOperation = std::function<bool(
      const std::filesystem::path&, const std::filesystem::path&,
      const std::filesystem::path&, const LoopSplitConfig&, LoopSplitResult*,
      const ProgressReporter&, const CancellationRequested&,
      const CommitStarted&, std::string*)>;
  using FileExportOperation = std::function<bool(
      const std::filesystem::path&, const std::filesystem::path&,
      const ProgressReporter&, const CancellationRequested&,
      const CommitStarted&, std::string*)>;

  struct Operations {
    MelodyOperation analyzeMelody;
    LoopSplitOperation splitLoop;
    playback_video_transcript::GenerationJob::Operation generateSubtitles;
    audio_separation::Job::Operation separateAudio;
    FileExportOperation exportAudio;
    FileExportOperation exportTranscriptText;
  };

  // Uses Radioify's production melody, loop-split, Vulkan transcript, and
  // native Windows NVIDIA separation backends.
  explicit Coordinator(AudioPlaybackRuntime& audioPlayback);
  // The operation boundary keeps lifecycle and presentation tests independent
  // from heavyweight media/GPU backends.
  explicit Coordinator(Operations operations);
  ~Coordinator() override;

  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  bool running() const;
  std::optional<TaskActivity> activity() const;
  std::optional<TaskCompletion> latestCompletion() const;

  playback_media_processing::RequestResult tryStartMelodyAnalysis(
      const std::filesystem::path& sourceFile, int trackIndex,
      const std::filesystem::path& outputFile);
  playback_media_processing::RequestResult tryStartLoopSplit(
      const std::filesystem::path& sourceFile,
      const std::filesystem::path& stingerOutput,
      const std::filesystem::path& loopOutput,
      const LoopSplitConfig& config);

  playback_media_processing::SourceState sourceStateFor(
      const std::filesystem::path& sourceFile) const override;
  playback_media_processing::RequestResult requestSubtitles(
      const std::filesystem::path& sourceFile) override;
  playback_media_processing::RequestResult requestSubtitleCancellation()
      override;
  playback_media_processing::RequestResult requestAudioExport(
      const std::filesystem::path& sourceFile) override;
  playback_media_processing::RequestResult requestTranscriptTextExport(
      const std::filesystem::path& sourceFile) override;
  playback_media_processing::RequestResult requestMediaExportCancellation()
      override;
  playback_media_processing::RequestResult requestAudioSeparation(
      const std::filesystem::path& sourceFile) override;
  playback_media_processing::RequestResult requestAudioSeparationCancellation()
      override;

  bool subtitleGenerationRunningFor(
      const std::filesystem::path& sourceFile) const;
  bool audioSeparationAvailableFor(
      const std::filesystem::path& sourceFile) const;
  bool audioSeparationRunningFor(
      const std::filesystem::path& sourceFile) const;
  bool hasSeparatedAudioFor(const std::filesystem::path& sourceFile) const;
  bool audioExportAvailableFor(
      const std::filesystem::path& sourceFile) const;
  bool transcriptTextExportAvailableFor(
      const std::filesystem::path& sourceFile) const;

  // Cancels only when the active task still has the identity observed by the
  // caller. This prevents a stale confirmation surface from cancelling a
  // newer task.
  bool cancelActive(TaskId expectedTask);

  // A lease makes foreground resource ownership follow the playback
  // lifecycle. ready() becomes true only after resource-intensive background
  // work has reached a verified suspension point.
  InteractivePlaybackLease acquireInteractivePlayback();

  PollResult poll();
  // One owner event fans in every worker family; UI loops never depend on the
  // number of concrete processing backends.
  NativeWaitHandle waitHandle() const;
  void shutdown();

 private:
  struct Backends {
    MelodyOperation analyzeMelody;
    LoopSplitOperation splitLoop;
    playback_video_transcript::GenerationJob::Operation generateSubtitles;
    audio_separation::Job::Operation separateAudio;
    FileExportOperation exportAudio;
    FileExportOperation exportTranscriptText;
  };
  explicit Coordinator(Backends backends);

  playback_media_processing::RequestResult tryStartFileExport(
      TaskKind kind, const std::filesystem::path& sourceFile,
      const std::filesystem::path& outputFile,
      const FileExportOperation& operation, const char* initialPhase,
      const char* fallbackError);
  bool collectReadyCompletions();
  std::optional<playback_media_processing::RequestError> startConflict();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace media_processing
