#include "audio/separation/operation.h"

#include <utility>

#include "audio/separation/separator.h"
#include "audio/separation/windows_ml_backend.h"

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
  WindowsMlBackendResolution resolution = resolveNvidiaWindowsMlBackend(
      ProviderProvisioningPolicy::ActivateInstalled);
  if (resolution.ready()) {
    return makeProductionOperation(std::move(resolution.backend));
  }

  std::string detail = resolution.detail.empty()
                           ? "Windows ML did not provide diagnostic detail."
                           : std::move(resolution.detail);
  return [detail = std::move(detail)](
             const std::filesystem::path&, const ArtifactPaths&,
             const Job::ProgressReporter&,
             const Job::DiagnosticReporter& reportDiagnostic,
             const ExecutionControl&, const Job::CommitStarted&,
             std::string* error) {
    const std::string message =
        "Native NVIDIA audio separation is unavailable. " + detail;
    audio_separation::reportDiagnostic(
        reportDiagnostic, DiagnosticLevel::Error, "nvidia-tensorrt-rtx",
        message);
    if (error) *error = message;
    return false;
  };
}

}  // namespace audio_separation
