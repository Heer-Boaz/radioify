#include "indexed_transcript_task.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "playback/video/transcript/document.h"
#include "playback/video/transcript/transcriber.h"
#include "runtime_helpers.h"

IndexedTranscriptTask::~IndexedTranscriptTask() { cancelAndJoin(); }

void IndexedTranscriptTask::reapFinished() {
  bool shouldJoin = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shouldJoin = !state_.running && worker_.joinable();
  }
  if (shouldJoin) worker_.join();
}

void IndexedTranscriptTask::cancelAndJoin() {
  cancelRequested_.store(true, std::memory_order_relaxed);
  if (worker_.joinable()) worker_.join();
}

IndexedTranscriptTaskSnapshot IndexedTranscriptTask::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

bool IndexedTranscriptTask::tryStart(const std::filesystem::path& videoPath) {
  reapFinished();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.running) return false;
  }

  const std::filesystem::path outputPath =
      playback_video_transcript::availableTranscriptPath(videoPath);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.running) return false;
    cancelRequested_.store(false, std::memory_order_relaxed);
    state_.running = true;
    state_.hasResult = false;
    state_.success = false;
    state_.progress = 0.0f;
    state_.phase = "Starting transcript";
    state_.status.clear();
    state_.sourceFile = videoPath;
    state_.outputFile = outputPath;

    try {
      worker_ = std::thread([this, videoPath, outputPath]() {
        std::string error;
        bool ok = false;
        try {
          ok = playback_video_transcript::createIndexedTranscript(
              videoPath, outputPath,
              [this](const playback_video_transcript::Progress& progress) {
                std::lock_guard<std::mutex> progressLock(mutex_);
                state_.progress =
                    std::max(state_.progress,
                             std::clamp(progress.fraction, 0.0f, 1.0f));
                state_.phase = progress.phase;
              },
              &cancelRequested_, &error);
        } catch (const std::exception& exception) {
          error = std::string("Transcript failed: ") + exception.what();
        } catch (...) {
          error = "Transcript failed unexpectedly.";
        }

        std::lock_guard<std::mutex> resultLock(mutex_);
        state_.running = false;
        state_.hasResult = true;
        state_.success = ok;
        state_.progress = ok ? 1.0f : state_.progress;
        state_.phase.clear();
        state_.status =
            ok ? "Saved " + toUtf8String(outputPath.filename())
               : (error.empty() ? "Transcript failed." : error);
      });
    } catch (const std::exception& exception) {
      state_.running = false;
      state_.hasResult = true;
      state_.success = false;
      state_.phase.clear();
      state_.status =
          std::string("Could not start transcript: ") + exception.what();
      return true;
    }
  }
  return true;
}
