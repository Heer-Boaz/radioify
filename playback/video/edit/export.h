#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "playback/video/edit/view.h"

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
  std::vector<SourceRange> keptRanges;
  // When supplied by a playback session, export the streams the user was
  // actually viewing/listening to. Negative indices retain the standalone
  // exporter's best-stream fallback.
  int videoStreamIndex = -1;
  int audioStreamIndex = -1;
};

struct ExportSnapshot {
  ExportState state = ExportState::Idle;
  double progress = 0.0;
  std::filesystem::path destinationPath;
  // Exact edit decision list owned by this asynchronous job.
  std::vector<SourceRange> keptRanges;
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
