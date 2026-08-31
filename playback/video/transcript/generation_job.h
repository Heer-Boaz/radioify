#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "core/native_wait_handle.h"
#include "core/wake_event.h"

namespace playback_video_transcript {

enum class GenerationJobState : uint8_t {
  Idle,
  Running,
  Cancelling,
  Publishing,
  Succeeded,
  Failed,
  Cancelled,
};

struct GenerationJobSnapshot {
  GenerationJobState state = GenerationJobState::Idle;
  float progress = 0.0f;
  std::string phase;
  std::string error;
  std::string diagnosticError;
  std::filesystem::path sourceFile;
  std::filesystem::path outputFile;

  bool running() const {
    return state == GenerationJobState::Running ||
           state == GenerationJobState::Cancelling ||
           state == GenerationJobState::Publishing;
  }
  bool cancelling() const {
    return state == GenerationJobState::Cancelling;
  }
  bool cancellable() const {
    return state == GenerationJobState::Running;
  }
  bool finished() const {
    return state == GenerationJobState::Succeeded ||
           state == GenerationJobState::Failed ||
           state == GenerationJobState::Cancelled;
  }
  bool succeeded() const {
    return state == GenerationJobState::Succeeded;
  }
};

// Owns the complete lifecycle of one background transcript-generation job.
// Consumers observe immutable state, receive each completion exactly once,
// and can wait on the change handle instead of polling worker internals.
class GenerationJob {
 public:
  using ProgressReporter = std::function<void(float, std::string)>;
  using CommitStarted = std::function<bool()>;
  using Operation = std::function<bool(
      const std::filesystem::path&, const std::filesystem::path&,
      const ProgressReporter&, const std::atomic<bool>*,
      const CommitStarted&, std::string*)>;

  // Uses Radioify's configured Whisper/Vulkan transcription operation.
  GenerationJob();
  static Operation productionOperation();
  static const char* productionEngineName();
  // The operation boundary also permits another transcription backend without
  // changing lifecycle, cancellation, or UI coordination code.
  explicit GenerationJob(Operation operation);
  GenerationJob(Operation operation, WakeNotifier ownerWake);
  ~GenerationJob();

  GenerationJob(const GenerationJob&) = delete;
  GenerationJob& operator=(const GenerationJob&) = delete;

  bool tryStart(const std::filesystem::path& videoPath);
  bool requestCancel();
  void cancelAndJoin();
  GenerationJobSnapshot snapshot() const;
  bool configured() const;
  std::optional<GenerationJobSnapshot> takeCompletion();
  bool consumeChanged();
  NativeWaitHandle nativeWaitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_transcript
