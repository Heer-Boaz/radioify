#include "audio/separation/operation.h"

#include <utility>

#include "audio/separation/separator.h"

namespace audio_separation {

Job::Operation makeModelOperation(std::filesystem::path modelPath,
                                  InferenceBackend backend) {
  return [modelPath = std::move(modelPath), backend = std::move(backend)](
             const std::filesystem::path& mediaPath,
             const ArtifactPaths& outputPaths,
             const Job::ProgressReporter& reportProgress,
             const Job::DiagnosticReporter& reportDiagnostic,
             const ExecutionControl& control,
             const Job::CommitStarted& outputCommitStarted,
             std::string* error) {
    return separateMediaAudioWithModel(
        mediaPath, modelPath, backend, outputPaths,
        [&](const Progress& progress) {
          reportProgress(progress.fraction, progress.phase);
        },
        reportDiagnostic,
        control, error, outputCommitStarted);
  };
}

Job::Operation makeModelOperation(std::filesystem::path modelPath) {
  return makeModelOperation(std::move(modelPath),
                            directMlInferenceBackend());
}

Job::Operation makeProductionOperation(InferenceBackend backend) {
  return [backend = std::move(backend)](
            const std::filesystem::path& mediaPath,
            const ArtifactPaths& outputPaths,
            const Job::ProgressReporter& reportProgress,
            const Job::DiagnosticReporter& reportDiagnostic,
            const ExecutionControl& control,
            const Job::CommitStarted& outputCommitStarted,
            std::string* error) {
    return separateMediaAudio(
        mediaPath, backend, outputPaths,
        [&](const Progress& progress) {
          reportProgress(progress.fraction, progress.phase);
        },
        reportDiagnostic,
        control, error, outputCommitStarted);
  };
}

Job::Operation makeProductionOperation() {
  return makeProductionOperation(directMlInferenceBackend());
}

}  // namespace audio_separation
