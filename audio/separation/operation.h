#pragma once

#include <filesystem>

#include "audio/separation/inference_backend.h"
#include "audio/separation/job.h"

namespace audio_separation {

// Production uses only the certified native Windows NVIDIA provider. Missing
// dependencies produce an actionable failure operation; this path never
// installs software and never falls back silently to DirectML or WSL.
Job::Operation makeProductionOperation();
// Explicit injection keeps tests and composition experiments independent from
// machine discovery. The separator itself never probes or installs software.
Job::Operation makeProductionOperation(InferenceBackend backend);

// Alternate models are an explicit diagnostic dependency and never ambient
// process configuration inherited by the application.
Job::Operation makeModelOperation(std::filesystem::path modelPath);
Job::Operation makeModelOperation(std::filesystem::path modelPath,
                                  InferenceBackend backend);

}  // namespace audio_separation
