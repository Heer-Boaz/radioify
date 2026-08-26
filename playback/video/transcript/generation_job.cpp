#include "playback/video/transcript/generation_job.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "core/runtime_helpers.h"
#include "playback/video/transcript/artifact.h"
#include "playback/video/transcript/transcriber.h"

namespace playback_video_transcript {

GenerationJob::~GenerationJob() { cancelAndJoin(); }

void GenerationJob::reapFinished() {
  bool shouldJoin = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shouldJoin = !state_.running && worker_.joinable();
  }
  if (shouldJoin) worker_.join();
}

void GenerationJob::cancelAndJoin() {
  requestCancel();
  if (worker_.joinable()) worker_.join();
}

bool GenerationJob::requestCancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!state_.running || state_.cancelRequested) return false;
  cancelRequested_.store(true, std::memory_order_relaxed);
  state_.cancelRequested = true;
  state_.phase = "Cancelling subtitle generation";
  return true;
}

GenerationJobSnapshot GenerationJob::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

bool GenerationJob::tryStart(const std::filesystem::path& videoPath) {
  const std::filesystem::path outputPath =
      transcriptPathForVideo(videoPath);
  if (videoPath.empty() || outputPath.empty()) return false;

  reapFinished();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.running) return false;
    cancelRequested_.store(false, std::memory_order_relaxed);
    state_.running = true;
    state_.hasResult = false;
    state_.success = false;
    state_.cancelRequested = false;
    state_.progress = 0.0f;
    state_.phase = "Starting subtitle generation";
    state_.status.clear();
    state_.sourceFile = videoPath;
    state_.outputFile = outputPath;

    try {
      worker_ = std::thread([this, videoPath, outputPath]() {
        std::string error;
        bool ok = false;
        try {
          ok = createIndexedTranscript(
              videoPath, outputPath, TranscriptPublishMode::ReplaceExisting,
              [this](const Progress& progress) {
                std::lock_guard<std::mutex> progressLock(mutex_);
                state_.progress =
                    std::max(state_.progress,
                             std::clamp(progress.fraction, 0.0f, 1.0f));
                if (!state_.cancelRequested) state_.phase = progress.phase;
              },
              &cancelRequested_, &error);
        } catch (const std::exception& exception) {
          error = std::string("Subtitle generation failed: ") +
                  exception.what();
        } catch (...) {
          error = "Subtitle generation failed unexpectedly.";
        }

        std::lock_guard<std::mutex> resultLock(mutex_);
        state_.running = false;
        state_.hasResult = true;
        state_.success = ok;
        const bool wasCancelled = state_.cancelRequested;
        state_.progress = ok ? 1.0f : state_.progress;
        state_.phase.clear();
        ++state_.resultRevision;
        state_.status =
            ok ? "Subtitles ready: " + toUtf8String(outputPath.filename())
               : (wasCancelled ? "Subtitle generation cancelled."
                               : (error.empty()
                                      ? "Subtitle generation failed."
                                      : error));
      });
    } catch (const std::exception& exception) {
      state_.running = false;
      state_.hasResult = true;
      state_.success = false;
      state_.phase.clear();
      ++state_.resultRevision;
      state_.status =
          std::string("Could not start subtitle generation: ") +
          exception.what();
      return true;
    }
  }
  return true;
}

}  // namespace playback_video_transcript
