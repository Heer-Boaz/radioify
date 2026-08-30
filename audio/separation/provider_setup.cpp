#include "audio/separation/provider_setup.h"

#include <utility>

#include "audio/separation/windows_ml_backend.h"

namespace audio_separation {

ProviderSetupOperation makeProductionProviderSetupOperation() {
  return [](const ProviderSetupProgress& reportProgress,
            const ProviderSetupCancellation& cancellationRequested) {
    WindowsMlProviderSetupResult setup = setupNvidiaWindowsMlBackend(
        reportProgress, cancellationRequested);
    ProviderSetupResult result;
    result.detail = std::move(setup.detail);
    switch (setup.outcome) {
      case WindowsMlProviderSetupOutcome::Ready:
        result.outcome = ProviderSetupOutcome::Ready;
        break;
      case WindowsMlProviderSetupOutcome::Cancelled:
        result.outcome = ProviderSetupOutcome::Cancelled;
        break;
      case WindowsMlProviderSetupOutcome::Failed:
        result.outcome = ProviderSetupOutcome::Failed;
        break;
    }
    return result;
  };
}

}  // namespace audio_separation
