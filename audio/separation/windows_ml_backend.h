#pragma once

#include <functional>
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
// dependency graph but never downloads it. ObserveOnly is a side-effect-free
// capability query; downloads are possible only through the explicit async
// setup operation below.
WindowsMlBackendResolution resolveNvidiaWindowsMlBackend(
    ProviderProvisioningPolicy policy);

enum class WindowsMlProviderSetupOutcome {
  Ready,
  Cancelled,
  Failed,
};

struct WindowsMlProviderSetupResult {
  WindowsMlProviderSetupOutcome outcome =
      WindowsMlProviderSetupOutcome::Failed;
  WindowsMlBackendResolution resolution;
  std::string detail;

  bool ready() const {
    return outcome == WindowsMlProviderSetupOutcome::Ready &&
           resolution.ready();
  }
};

using WindowsMlProviderSetupProgress =
    std::function<void(float, std::string)>;
using WindowsMlProviderSetupCancellation = std::function<bool()>;

// Installs or activates the certified NVIDIA provider through Windows ML's
// asynchronous provisioning contract. Production callers must invoke this
// only after explicit user consent. Progress and cancellation are delivered on
// the calling worker thread; Windows ML callbacks never enter application/UI
// state directly.
WindowsMlProviderSetupResult setupNvidiaWindowsMlBackend(
    const WindowsMlProviderSetupProgress& reportProgress,
    const WindowsMlProviderSetupCancellation& cancellationRequested);

}  // namespace audio_separation
