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

enum class InstalledProviderPolicy {
  ObserveOnly,
  Activate,
};

struct WindowsMlBackendResolution {
  WindowsMlBackendStatus status = WindowsMlBackendStatus::Unavailable;
  InferenceBackend backend;
  std::string version;
  std::string detail;

  bool ready() const { return status == WindowsMlBackendStatus::Ready; }
};

// Resolves the certified NVIDIA provider through Windows ML's catalog. The
// function never downloads a missing provider. Activate may add an already
// installed package to this process' dependency graph; ObserveOnly is a
// side-effect-free capability query.
WindowsMlBackendResolution resolveNvidiaWindowsMlBackend(
    InstalledProviderPolicy policy);

}  // namespace audio_separation
