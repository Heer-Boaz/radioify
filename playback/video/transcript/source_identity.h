#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "core/file_instance.h"

namespace playback_video_transcript {

// In-memory guard for one running transcription, never a persisted sidecar.
struct TranscriptSourceIdentity {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type modified;
  FileInstanceIdentity instance;
};

std::optional<TranscriptSourceIdentity> captureTranscriptSourceIdentity(
    const std::filesystem::path& path, std::string* error = nullptr);
bool transcriptSourceMatches(const TranscriptSourceIdentity& expected,
                             const std::filesystem::path& path);

}  // namespace playback_video_transcript
