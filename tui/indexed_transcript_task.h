#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

struct IndexedTranscriptTaskState {
  std::mutex mutex;
  std::thread worker;
  std::atomic<bool> cancelRequested{false};
  bool running = false;
  bool hasResult = false;
  bool success = false;
  float progress = 0.0f;
  std::string phase;
  std::string status;
  std::filesystem::path sourceFile;
  std::filesystem::path outputFile;
};

void cleanupIndexedTranscriptWorker(IndexedTranscriptTaskState& state);
void cancelAndJoinIndexedTranscriptWorker(IndexedTranscriptTaskState& state);
void startIndexedTranscript(const std::filesystem::path& videoPath,
                            IndexedTranscriptTaskState& state);
