#include "audio/separation/job.h"

#include "audio/separation/separator.h"

namespace audio_separation {

Job::Operation Job::productionOperation() {
  return [](const std::filesystem::path& mediaPath,
            const ArtifactPaths& outputPaths,
            const ProgressReporter& reportProgress,
            const std::atomic<bool>* cancelRequested, std::string* error) {
    return separateMediaAudio(
        mediaPath, outputPaths,
        [&](const Progress& progress) {
          reportProgress(progress.fraction, progress.phase);
        },
        cancelRequested, error);
  };
}

Job::Job() : Job(productionOperation()) {}

}  // namespace audio_separation
