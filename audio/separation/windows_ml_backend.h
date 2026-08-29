#pragma once

#include <string>

#include "audio/separation/inference_backend.h"

namespace audio_separation {

enum class WindowsMlBackendStatus {
  Ready,
  Installed,
  InstallationRequired,
  Unavailable,
  Failed,
};

enum class ProviderProvisioningPolicy {
  ObserveOnly,
  ActivateInstalled,
  InstallIfMissing,
};

struct WindowsMlBackendResolution {
  WindowsMlBackendStatus status = WindowsMlBackendStatus::Unavailable;
  InferenceBackend backend;
  std::string version;
  std::string detail;

  bool ready() const { return status == WindowsMlBackendStatus::Ready; }
};

// Resolves the certified NVIDIA provider through Windows ML's catalog. The
// ActivateInstalled may add an already installed package to this process'
// dependency graph but never downloads it. InstallIfMissing is reserved for an
// explicit user-approved provisioning workflow. ObserveOnly is a side-effect-
// free capability query.
WindowsMlBackendResolution resolveNvidiaWindowsMlBackend(
    ProviderProvisioningPolicy policy);

}  // namespace audio_separation
