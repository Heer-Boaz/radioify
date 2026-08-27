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
#include "playback/video/edit/decision_list.h"

namespace playback_video_edit {

enum class ExportState : uint8_t {
  Idle,
  Running,
  Succeeded,
  Failed,
  Cancelled,
};

struct ExportRequest {
  std::filesystem::path sourcePath;
  std::filesystem::path destinationPath;
  DecisionList decisions;
  // The edited program uses the video stream the user was viewing. Every
  // source audio/subtitle stream is retained; track selection must never act
  // as an implicit export-time deletion policy.
  int videoStreamIndex = -1;
};

struct ExportSnapshot {
  ExportState state = ExportState::Idle;
  double progress = 0.0;
  std::filesystem::path destinationPath;
  // Exact edit decision list owned by this asynchronous job.
  DecisionList decisions;
  std::string videoEncoder;
  std::string error;

  bool running() const { return state == ExportState::Running; }
  bool finished() const {
    return state == ExportState::Succeeded || state == ExportState::Failed ||
           state == ExportState::Cancelled;
  }
};

struct ExportResult {
  ExportState state = ExportState::Failed;
  std::string videoEncoder;
  std::string error;
};

// Selects a non-existing sibling output. The source path is never returned,
// and existing edit exports are never reused.
std::filesystem::path uniqueEditedOutputPath(
    const std::filesystem::path& sourcePath);

// Worker boundary owned by the video-edit workspace. The request and progress
// are published as immutable snapshots, and each terminal result is consumed
// exactly once before another export can replace it.
class Exporter {
 public:
  using ProgressReporter = std::function<void(double)>;
  using Operation = std::function<ExportResult(
      const ExportRequest&, const std::atomic<bool>*,
      const ProgressReporter&)>;

  // Uses Radioify's FFmpeg export pipeline.
  Exporter();
  explicit Exporter(WakeNotifier ownerWake);
  // The operation boundary keeps worker lifecycle and workspace coordination
  // independent from the concrete encoder pipeline.
  explicit Exporter(Operation operation);
  Exporter(Operation operation, WakeNotifier ownerWake);
  ~Exporter();

  Exporter(const Exporter&) = delete;
  Exporter& operator=(const Exporter&) = delete;

  bool start(ExportRequest request);
  bool cancel();
  void stop();

  ExportSnapshot snapshot() const;
  std::optional<ExportSnapshot> takeCompletion();
  bool consumeChanged();
  NativeWaitHandle nativeWaitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_edit
