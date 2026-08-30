#pragma once

#include <functional>
#include <string>

namespace audio_separation {

enum class ProviderSetupOutcome {
  Ready,
  Cancelled,
  Failed,
};

struct ProviderSetupResult {
  ProviderSetupOutcome outcome = ProviderSetupOutcome::Failed;
  std::string detail;

  bool ready() const { return outcome == ProviderSetupOutcome::Ready; }
};

using ProviderSetupProgress = std::function<void(float, std::string)>;
using ProviderSetupCancellation = std::function<bool()>;
using ProviderSetupOperation = std::function<ProviderSetupResult(
    const ProviderSetupProgress&, const ProviderSetupCancellation&)>;

// Explicit, user-approved setup operation for the optional production
// provider. Capability discovery remains side-effect-free.
ProviderSetupOperation makeProductionProviderSetupOperation();

}  // namespace audio_separation
