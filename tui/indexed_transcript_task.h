#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

struct IndexedTranscriptTaskSnapshot {
  bool running = false;
  bool hasResult = false;
  bool success = false;
  float progress = 0.0f;
  std::string phase;
  std::string status;
  std::filesystem::path sourceFile;
  std::filesystem::path outputFile;
};

class IndexedTranscriptTask {
 public:
  IndexedTranscriptTask() = default;
  ~IndexedTranscriptTask();

  IndexedTranscriptTask(const IndexedTranscriptTask&) = delete;
  IndexedTranscriptTask& operator=(const IndexedTranscriptTask&) = delete;

  bool tryStart(const std::filesystem::path& videoPath);
  void reapFinished();
  void cancelAndJoin();
  IndexedTranscriptTaskSnapshot snapshot() const;

 private:
  mutable std::mutex mutex_;
  std::thread worker_;
  std::atomic<bool> cancelRequested_{false};
  IndexedTranscriptTaskSnapshot state_;
};
