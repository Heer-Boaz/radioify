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
#include "core/native_wait_handle.h"
#include "playback/video/transcript/generation_job.h"

namespace media_processing {

enum class TaskKind {
  MelodyAnalysis,
  LoopSplit,
  SubtitleGeneration,
  AudioSeparation,
};

enum class TaskOutcome {
  Succeeded,
  Failed,
  Cancelled,
};

struct TaskActivity {
  TaskKind kind = TaskKind::MelodyAnalysis;
  std::filesystem::path sourceFile;
  float progress = 0.0f;
  std::string phase;
  bool cancelling = false;
  bool cancellable = false;
};

struct TaskCompletion {
  TaskKind kind = TaskKind::MelodyAnalysis;
  TaskOutcome outcome = TaskOutcome::Failed;
  std::filesystem::path sourceFile;
  std::filesystem::path outputFile;
  std::string detail;

  bool succeeded() const { return outcome == TaskOutcome::Succeeded; }
};

struct PollResult {
  bool changed = false;
  std::vector<TaskCompletion> completions;
};

// Owns Radioify's mutually-exclusive, offline media-processing jobs. The TUI
// observes one common activity/completion contract instead of understanding
// every worker's lifecycle and synchronization details.
class Coordinator {
 public:
  using MelodyProgressReporter = std::function<void(float)>;
  using MelodyOperation = std::function<bool(
      const std::filesystem::path&, int, const std::filesystem::path&,
      const MelodyProgressReporter&, std::string*)>;
  using LoopSplitOperation = std::function<bool(
      const std::filesystem::path&, const std::filesystem::path&,
      const std::filesystem::path&, const LoopSplitConfig&, LoopSplitResult*,
      std::string*)>;

  struct Operations {
    MelodyOperation analyzeMelody;
    LoopSplitOperation splitLoop;
    playback_video_transcript::GenerationJob::Operation generateSubtitles;
    audio_separation::Job::Operation separateAudio;
    bool audioSeparationAvailable = false;
  };

  // Uses Radioify's production melody, loop-split, Vulkan transcript, and
  // DirectML separation backends.
  Coordinator();
  // The operation boundary keeps lifecycle and presentation tests independent
  // from heavyweight media/GPU backends.
  explicit Coordinator(Operations operations);
  ~Coordinator();

  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  bool running() const;
  std::optional<TaskActivity> activity() const;
  std::optional<TaskCompletion> latestCompletion() const;

  bool tryStartMelodyAnalysis(const std::filesystem::path& sourceFile,
                              int trackIndex,
                              const std::filesystem::path& outputFile);
  bool tryStartLoopSplit(const std::filesystem::path& sourceFile,
                         const std::filesystem::path& stingerOutput,
                         const std::filesystem::path& loopOutput,
                         const LoopSplitConfig& config);
  bool tryStartSubtitleGeneration(const std::filesystem::path& sourceFile);
  bool tryStartAudioSeparation(const std::filesystem::path& sourceFile);

  bool subtitleGenerationRunningFor(
      const std::filesystem::path& sourceFile) const;
  bool audioSeparationAvailableFor(
      const std::filesystem::path& sourceFile) const;
  bool audioSeparationRunningFor(
      const std::filesystem::path& sourceFile) const;
  bool hasSeparatedAudioFor(const std::filesystem::path& sourceFile) const;

  bool cancelSubtitleGeneration();
  bool cancelAudioSeparation();
  bool cancelActive();

  PollResult poll();
  std::vector<NativeWaitHandle> waitHandles() const;
  void shutdown();

 private:
  struct Backends {
    MelodyOperation analyzeMelody;
    LoopSplitOperation splitLoop;
    std::unique_ptr<playback_video_transcript::GenerationJob> subtitles;
    std::unique_ptr<audio_separation::Job> audioSeparation;
    bool audioSeparationAvailable = false;
  };
  explicit Coordinator(Backends backends);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace media_processing
