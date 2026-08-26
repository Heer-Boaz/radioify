#pragma once

#include <cstdint>
#include <filesystem>

namespace playback_video_sidecars {

// Classifies only sidecars that can be associated automatically with a video.
// The complete video stem must match; arbitrary files from the same directory
// are never inferred to belong to the video.
enum class AutomaticSubtitleKind : uint8_t {
  None,
  Subtitle,
  IndexedTranscript,
};

AutomaticSubtitleKind classifyAutomaticSubtitleSidecar(
    const std::filesystem::path& videoPath,
    const std::filesystem::path& candidatePath);

bool isIndexedTranscriptSidecar(
    const std::filesystem::path& videoPath,
    const std::filesystem::path& candidatePath);

}  // namespace playback_video_sidecars
