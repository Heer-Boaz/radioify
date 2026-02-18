#ifndef LOOPSPLIT_UI_H
#define LOOPSPLIT_UI_H

#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

struct LoopSplitTaskState {
  std::mutex mutex;
  std::thread worker;
  bool running = false;
  bool hasResult = false;
  bool success = false;
  float progress = 0.0f;
  std::string status;
  std::filesystem::path sourceFile;
  std::filesystem::path stingerFile;
  std::filesystem::path loopFile;
};

void cleanupLoopSplitExportWorker(LoopSplitTaskState& state);

void joinLoopSplitExportWorker(LoopSplitTaskState& state);

void startLoopSplitExport(const std::filesystem::path& entryPath,
                         const std::string& outputArg,
                         LoopSplitTaskState& state);

#endif  // LOOPSPLIT_UI_H
