#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "audio/separation/artifact.h"
#include "core/native_wait_handle.h"
#include "core/wake_event.h"

namespace audio_separation {

enum class JobState : std::uint8_t {
  Idle,
  Running,
  Cancelling,
  Succeeded,
  Failed,
  Cancelled,
};

struct JobSnapshot {
  JobState state = JobState::Idle;
  float progress = 0.0f;
  std::string phase;
  std::string error;
  std::filesystem::path sourceFile;
  ArtifactPaths outputFiles{};

  bool running() const {
    return state == JobState::Running || state == JobState::Cancelling;
  }
  bool cancelling() const { return state == JobState::Cancelling; }
  bool finished() const {
    return state == JobState::Succeeded || state == JobState::Failed ||
           state == JobState::Cancelled;
  }
  bool succeeded() const { return state == JobState::Succeeded; }
};

// Owns one background separation operation and exposes immutable snapshots.
// Completions are delivered once, while the wait handle lets the TUI sleep
// without reaching into worker-thread state.
class Job {
 public:
  using ProgressReporter = std::function<void(float, std::string)>;
  using Operation = std::function<bool(
      const std::filesystem::path&, const ArtifactPaths&,
      const ProgressReporter&, const std::atomic<bool>*, std::string*)>;

  explicit Job(Operation operation);
  Job(Operation operation, WakeNotifier ownerWake);
  ~Job();

  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;

  bool tryStart(const std::filesystem::path& mediaPath);
  bool requestCancel();
  void cancelAndJoin();
  JobSnapshot snapshot() const;
  bool configured() const;
  std::optional<JobSnapshot> takeCompletion();
  bool consumeChanged();
  NativeWaitHandle nativeWaitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace audio_separation
