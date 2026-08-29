#pragma once

#include <filesystem>

#include "audio/separation/inference_backend.h"
#include "audio/separation/job.h"

namespace audio_separation {

// Production composition owns the provider decision. The separator receives a
// fully resolved backend and never probes or changes machine configuration.
Job::Operation makeProductionOperation();
Job::Operation makeProductionOperation(InferenceBackend backend);

// Alternate models are an explicit diagnostic dependency and never ambient
// process configuration inherited by the application.
Job::Operation makeModelOperation(std::filesystem::path modelPath);
Job::Operation makeModelOperation(std::filesystem::path modelPath,
                                  InferenceBackend backend);

}  // namespace audio_separation
