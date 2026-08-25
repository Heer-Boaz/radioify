#include "indexed_transcript_task.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "playback/video/transcript/document.h"
#include "playback/video/transcript/transcriber.h"
#include "runtime_helpers.h"

void cleanupIndexedTranscriptWorker(IndexedTranscriptTaskState& state) {
  bool shouldJoin = false;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    shouldJoin = !state.running && state.worker.joinable();
  }
  if (shouldJoin) state.worker.join();
}

void cancelAndJoinIndexedTranscriptWorker(IndexedTranscriptTaskState& state) {
  state.cancelRequested.store(true, std::memory_order_relaxed);
  if (state.worker.joinable()) state.worker.join();
}

void startIndexedTranscript(const std::filesystem::path& videoPath,
                            IndexedTranscriptTaskState& state) {
  cleanupIndexedTranscriptWorker(state);
  const std::filesystem::path outputPath =
      playback_video_transcript::availableTranscriptPath(videoPath);
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.running) return;
    state.cancelRequested.store(false, std::memory_order_relaxed);
    state.running = true;
    state.hasResult = false;
    state.success = false;
    state.progress = 0.0f;
    state.phase = "Starting transcript";
    state.status.clear();
    state.sourceFile = videoPath;
    state.outputFile = outputPath;
  }

  state.worker = std::thread([videoPath, outputPath, &state]() {
    std::string error;
    bool ok = false;
    try {
      ok = playback_video_transcript::createIndexedTranscript(
          videoPath, outputPath,
          [&state](const playback_video_transcript::Progress& progress) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.progress =
                std::max(state.progress,
                         std::clamp(progress.fraction, 0.0f, 1.0f));
            state.phase = progress.phase;
          },
          &state.cancelRequested, &error);
    } catch (const std::exception& exception) {
      error = std::string("Transcript failed: ") + exception.what();
    } catch (...) {
      error = "Transcript failed unexpectedly.";
    }

    std::lock_guard<std::mutex> lock(state.mutex);
    state.running = false;
    state.hasResult = true;
    state.success = ok;
    state.progress = ok ? 1.0f : state.progress;
    state.phase.clear();
    state.status = ok ? "Saved " + toUtf8String(outputPath.filename())
                      : (error.empty() ? "Transcript failed." : error);
  });
}
