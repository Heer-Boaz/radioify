#include "loopsplit_ui.h"

#include <string>

#include "loopsplit.h"
#include "loopsplit_cli.h"
#include "ui_helpers.h"

void cleanupLoopSplitExportWorker(LoopSplitTaskState& state) {
  bool shouldJoin = false;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    shouldJoin = !state.running && state.worker.joinable();
  }
  if (shouldJoin) {
    state.worker.join();
  }
}

void joinLoopSplitExportWorker(LoopSplitTaskState& state) {
  if (state.worker.joinable()) {
    state.worker.join();
  }
}

void startLoopSplitExport(const std::filesystem::path& entryPath,
                          const std::string& outputArg,
                          const LoopSplitConfig& splitConfig,
                          LoopSplitTaskState& state) {
  const auto outputPaths = resolveSplitOutputPaths(entryPath, outputArg);
  const std::filesystem::path& stingerOutput = outputPaths.first;
  const std::filesystem::path& loopOutput = outputPaths.second;

  bool shouldJoin = false;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    shouldJoin = !state.running && state.worker.joinable();
  }
  if (shouldJoin) {
    state.worker.join();
  }

  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.running) {
      return;
    }
    state.running = true;
    state.hasResult = false;
    state.success = false;
    state.progress = 0.0f;
    state.status.clear();
    state.sourceFile = entryPath;
    state.stingerFile = stingerOutput;
    state.loopFile = loopOutput;
  }

  state.worker =
      std::thread([entryPath, stingerOutput, loopOutput, splitConfig, &state]() {
        LoopSplitResult splitResult;
        std::string error;
        bool ok = splitAudioIntoLoopFiles(entryPath, stingerOutput, loopOutput,
                                         splitConfig, &splitResult, &error);
        std::lock_guard<std::mutex> lock(state.mutex);
        state.running = false;
        state.hasResult = true;
        state.success = ok;
        state.progress = ok ? 1.0f : state.progress;
        if (ok) {
          if (splitResult.hasStinger) {
            state.status =
                "Saved " + toUtf8String(stingerOutput.filename()) + " and " +
                toUtf8String(loopOutput.filename());
          } else {
            state.status = "Saved " + toUtf8String(loopOutput.filename()) + " only";
          }
        } else {
          state.status = error.empty() ? "Loop split failed." : error;
        }
      });
}
