#include "audio/separation/operation.h"

#include <utility>

#include "audio/separation/separator.h"

namespace audio_separation {

Job::Operation makeModelOperation(std::filesystem::path modelPath) {
  return [modelPath = std::move(modelPath)](
             const std::filesystem::path& mediaPath,
             const ArtifactPaths& outputPaths,
             const Job::ProgressReporter& reportProgress,
             const Job::DiagnosticReporter& reportDiagnostic,
             const ExecutionControl& control, std::string* error) {
    return separateMediaAudioWithModel(
        mediaPath, modelPath, outputPaths,
        [&](const Progress& progress) {
          reportProgress(progress.fraction, progress.phase);
        },
        reportDiagnostic,
        control, error);
  };
}

Job::Operation makeProductionOperation() {
  return [](const std::filesystem::path& mediaPath,
            const ArtifactPaths& outputPaths,
            const Job::ProgressReporter& reportProgress,
            const Job::DiagnosticReporter& reportDiagnostic,
            const ExecutionControl& control, std::string* error) {
    return separateMediaAudio(
        mediaPath, outputPaths,
        [&](const Progress& progress) {
          reportProgress(progress.fraction, progress.phase);
        },
        reportDiagnostic,
        control, error);
  };
}

}  // namespace audio_separation
