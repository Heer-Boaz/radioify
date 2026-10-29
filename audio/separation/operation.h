#pragma once

#include <filesystem>

#include "audio/separation/inference_backend.h"
#include "audio/separation/operation_binding.h"

namespace audio_separation {

// Side-effect-free production capability discovery. It never installs a
// provider, starts WSL, or selects DirectML. Activation of an already installed
// native provider is deferred to the background operation.
OperationBinding resolveProductionOperation();

// Explicit backend injection keeps tests and diagnostic experiments
// independent from machine discovery. The default model is cached on first use;
// the separator itself never probes or installs an inference provider.
Job::Operation makeDefaultModelOperation(InferenceBackend backend);

// Alternate models are an explicit diagnostic dependency and never ambient
// process configuration inherited by the application.
Job::Operation makeModelOperation(std::filesystem::path modelPath,
                                  InferenceBackend backend);

}  // namespace audio_separation
