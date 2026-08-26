#include "playback/video/transcript/generation_job.h"

#include "playback/video/transcript/transcriber.h"

namespace playback_video_transcript {

GenerationJob::GenerationJob()
    : GenerationJob([](const std::filesystem::path& videoPath,
                       const std::filesystem::path& outputPath,
                       const ProgressReporter& reportProgress,
                       const std::atomic<bool>* cancelRequested,
                       std::string* error) {
        return createIndexedTranscript(
            videoPath, outputPath, TranscriptPublishMode::ReplaceExisting,
            [&](const Progress& progress) {
              reportProgress(progress.fraction, progress.phase);
            },
            cancelRequested, error);
      }) {}

}  // namespace playback_video_transcript
