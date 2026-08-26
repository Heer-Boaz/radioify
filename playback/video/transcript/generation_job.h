#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace playback_video_transcript {

struct GenerationJobSnapshot {
  bool running = false;
  bool hasResult = false;
  bool success = false;
  bool cancelRequested = false;
  float progress = 0.0f;
  uint64_t resultRevision = 0;
  std::string phase;
  std::string status;
  std::filesystem::path sourceFile;
  std::filesystem::path outputFile;
};

// Owns the complete lifecycle of one background transcript generation job.
// UI surfaces consume immutable snapshots and never coordinate the worker or
// its publication path themselves.
class GenerationJob {
 public:
  GenerationJob() = default;
  ~GenerationJob();

  GenerationJob(const GenerationJob&) = delete;
  GenerationJob& operator=(const GenerationJob&) = delete;

  bool tryStart(const std::filesystem::path& videoPath);
  bool requestCancel();
  void reapFinished();
  void cancelAndJoin();
  GenerationJobSnapshot snapshot() const;

 private:
  mutable std::mutex mutex_;
  std::thread worker_;
  std::atomic<bool> cancelRequested_{false};
  GenerationJobSnapshot state_;
};

}  // namespace playback_video_transcript
