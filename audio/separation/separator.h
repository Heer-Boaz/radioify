#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "audio/separation/artifact.h"
#include "audio/separation/diagnostics.h"
#include "audio/separation/execution_control.h"

namespace audio_separation {

struct Progress {
  float fraction = 0.0f;
  std::string phase;
};

using ProgressCallback = std::function<void(const Progress&)>;
using OutputCommitStarted = std::function<bool()>;

bool separateMediaAudio(const std::filesystem::path& mediaPath,
                        const ArtifactPaths& outputPaths,
                        const ProgressCallback& onProgress,
                        const DiagnosticReporter& diagnostics,
                        const ExecutionControl& control,
                        std::string* error,
                        const OutputCommitStarted& outputCommitStarted = {});

// Explicit model selection is reserved for diagnostic tools and tests.
// Production playback uses the bundled, build-verified model above.
bool separateMediaAudioWithModel(
    const std::filesystem::path& mediaPath,
    const std::filesystem::path& modelPath,
    const ArtifactPaths& outputPaths,
    const ProgressCallback& onProgress,
    const DiagnosticReporter& diagnostics,
    const ExecutionControl& control,
    std::string* error,
    const OutputCommitStarted& outputCommitStarted = {});

}  // namespace audio_separation
