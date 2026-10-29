#pragma once

#include "audio/separation/separator.h"

namespace audio_separation {

// Called by an explicit separation job, never by availability/UI queries.
bool ensureSeparationModel(std::filesystem::path *modelPath,
                           const ProgressCallback &onProgress,
                           const ExecutionControl &control, std::string *error);

} // namespace audio_separation
