#pragma once

#include <filesystem>
#include <string>

namespace playback_media_processing {

enum class Operation {
  SubtitleGeneration,
  AudioSeparation,
};

enum class Outcome {
  Succeeded,
  Failed,
  Cancelled,
};

struct Completion {
  Operation operation = Operation::SubtitleGeneration;
  Outcome outcome = Outcome::Failed;
  std::filesystem::path sourceFile;
  std::filesystem::path outputFile;
  std::string detail;

  bool succeeded() const { return outcome == Outcome::Succeeded; }
};

struct SourceState {
  bool backgroundTaskRunning = false;
  bool subtitleGenerationRunning = false;
  bool audioSeparationAvailable = false;
  bool audioSeparationRunning = false;
  bool separatedAudioExists = false;
};

// Narrow application service consumed by playback surfaces. Implementations
// own task lifecycle and source-state synchronization; callers only express
// user intent.
class Service {
 public:
  virtual ~Service() = default;

  virtual SourceState sourceStateFor(
      const std::filesystem::path& sourceFile) const = 0;
  virtual bool requestSubtitles(
      const std::filesystem::path& sourceFile) = 0;
  virtual bool requestSubtitleCancellation() = 0;
  virtual bool requestAudioSeparation(
      const std::filesystem::path& sourceFile) = 0;
  virtual bool requestAudioSeparationCancellation() = 0;
};

}  // namespace playback_media_processing
