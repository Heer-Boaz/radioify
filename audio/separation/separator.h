#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

#include "audio/separation/artifact.h"

namespace audio_separation {

struct Progress {
  float fraction = 0.0f;
  std::string phase;
};

using ProgressCallback = std::function<void(const Progress&)>;

bool separateMediaAudio(const std::filesystem::path& mediaPath,
                        const ArtifactPaths& outputPaths,
                        const ProgressCallback& onProgress,
                        const std::atomic<bool>* cancelRequested,
                        std::string* error);

}  // namespace audio_separation
