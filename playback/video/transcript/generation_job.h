#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "core/native_wait_handle.h"

namespace playback_video_transcript {

enum class GenerationJobState : uint8_t {
  Idle,
  Running,
  Cancelling,
  Succeeded,
  Failed,
  Cancelled,
};

struct GenerationJobSnapshot {
  GenerationJobState state = GenerationJobState::Idle;
  float progress = 0.0f;
  std::string phase;
  std::string error;
  std::filesystem::path sourceFile;
  std::filesystem::path outputFile;

  bool running() const {
    return state == GenerationJobState::Running ||
           state == GenerationJobState::Cancelling;
  }
  bool cancelling() const {
    return state == GenerationJobState::Cancelling;
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

// Owns the complete lifecycle of one background subtitle-generation job.
// Consumers observe immutable state, receive each completion exactly once,
// and can wait on the change handle instead of polling worker internals.
class GenerationJob {
 public:
  using ProgressReporter = std::function<void(float, std::string)>;
  using Operation = std::function<bool(
      const std::filesystem::path&, const std::filesystem::path&,
      const ProgressReporter&, const std::atomic<bool>*, std::string*)>;

  // Uses Radioify's configured Whisper/Vulkan transcription operation.
  GenerationJob();
  // The operation boundary also permits another transcription backend without
  // changing lifecycle, cancellation, or UI coordination code.
  explicit GenerationJob(Operation operation);
  ~GenerationJob();

  GenerationJob(const GenerationJob&) = delete;
  GenerationJob& operator=(const GenerationJob&) = delete;

  bool tryStart(const std::filesystem::path& videoPath);
  bool requestCancel();
  void cancelAndJoin();
  GenerationJobSnapshot snapshot() const;
  std::optional<GenerationJobSnapshot> takeCompletion();
  bool consumeChanged();
  NativeWaitHandle nativeWaitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_transcript
