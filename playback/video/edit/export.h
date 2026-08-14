#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

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

// Selects a non-existing sibling output. The source path is never returned,
// and existing edit exports are never reused.
std::filesystem::path uniqueEditedOutputPath(
    const std::filesystem::path& sourcePath);

class Exporter {
 public:
  Exporter();
  ~Exporter();

  Exporter(const Exporter&) = delete;
  Exporter& operator=(const Exporter&) = delete;

  bool start(ExportRequest request);
  void cancel();
  void stop();

  ExportSnapshot snapshot() const;
  bool consumeChanged();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_edit
