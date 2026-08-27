#pragma once

#include <filesystem>

#include "audio/separation/job.h"

namespace audio_separation {

// Production composition is pinned to Radioify's bundled DirectML model.
Job::Operation makeProductionOperation();

// Alternate models are an explicit diagnostic dependency and never ambient
// process configuration inherited by the application.
Job::Operation makeModelOperation(std::filesystem::path modelPath);

}  // namespace audio_separation
