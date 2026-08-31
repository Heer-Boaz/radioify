#include "audio/separation/operation.h"

#include <utility>

#include "audio/separation/separator.h"
#include "audio/separation/windows_ml_backend.h"

namespace audio_separation {

namespace {

OperationAvailability availabilityFor(WindowsMlBackendStatus status) {
  switch (status) {
    case WindowsMlBackendStatus::Ready:
    case WindowsMlBackendStatus::Installed:
      return OperationAvailability::Ready;
    case WindowsMlBackendStatus::InstallationRequired:
      return OperationAvailability::SetupRequired;
    case WindowsMlBackendStatus::Unavailable:
      return OperationAvailability::Unavailable;
    case WindowsMlBackendStatus::Failed:
      return OperationAvailability::Failed;
  }
  return OperationAvailability::Failed;
}

Job::Operation makeNativeNvidiaOperation() {
  return [](const std::filesystem::path& mediaPath,
            const ArtifactPaths& outputPaths,
            const Job::ProgressReporter& reportProgress,
            const Job::DiagnosticReporter& reportDiagnostic,
            const ExecutionControl& control,
            const Job::CommitStarted& outputCommitStarted,
            std::string* error) {
    reportProgress(0.0f, "Starting native NVIDIA audio separation");
    WindowsMlBackendResolution resolution = resolveNvidiaWindowsMlBackend(
        ProviderProvisioningPolicy::ActivateInstalled);
    if (!resolution.ready()) {
      const std::string detail =
          resolution.detail.empty()
              ? "Windows ML did not provide diagnostic detail."
              : std::move(resolution.detail);
      const std::string message =
          "Native NVIDIA audio separation is unavailable. " + detail;
      audio_separation::reportDiagnostic(
          reportDiagnostic, DiagnosticLevel::Error, "nvidia-tensorrt-rtx",
          message);
      if (error) *error = message;
      return false;
    }
    const Job::Operation operation =
        makeBundledModelOperation(std::move(resolution.backend));
    return operation(mediaPath, outputPaths, reportProgress, reportDiagnostic,
                     control, outputCommitStarted, error);
  };
}

}  // namespace

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

Job::Operation makeBundledModelOperation(InferenceBackend backend) {
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

OperationBinding resolveProductionOperation() {
  const WindowsMlBackendResolution observed = resolveNvidiaWindowsMlBackend(
      ProviderProvisioningPolicy::ObserveOnly);
  const OperationAvailability availability =
      availabilityFor(observed.status);
  if (availability == OperationAvailability::Ready) {
    return OperationBinding::ready(makeNativeNvidiaOperation(),
                                   "NVIDIA TensorRT-RTX (native Windows)");
  }
  return OperationBinding::unavailable(availability, observed.detail);
}

}  // namespace audio_separation
