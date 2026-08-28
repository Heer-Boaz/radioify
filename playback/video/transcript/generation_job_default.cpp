#include "playback/video/transcript/generation_job.h"

#include "playback/video/transcript/transcriber.h"

namespace playback_video_transcript {

GenerationJob::Operation GenerationJob::productionOperation() {
  return [](const std::filesystem::path& videoPath,
            const std::filesystem::path& outputPath,
            const ProgressReporter& reportProgress,
            const std::atomic<bool>* cancelRequested,
            const CommitStarted& outputCommitStarted,
            std::string* error) {
    return createIndexedTranscript(
        videoPath, outputPath, TranscriptPublishMode::ReplaceExisting,
        [&](const Progress& progress) {
          reportProgress(progress.fraction, progress.phase);
        },
        cancelRequested, error, outputCommitStarted);
  };
}

GenerationJob::GenerationJob() : GenerationJob(productionOperation()) {}

}  // namespace playback_video_transcript
