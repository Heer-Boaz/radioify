#pragma once

#include <filesystem>

namespace playback_video_transcript {

// Radioify owns one active transcript artifact next to each video. New
// transcripts are always published to this canonical path.
std::filesystem::path transcriptPathForVideo(
    const std::filesystem::path& videoPath);

// Resolves the transcript consumed by playback and analysis. The canonical
// artifact wins whenever it exists. Numbered files from older Radioify builds
// are considered only as a migration fallback, newest first.
std::filesystem::path activeTranscriptPathForVideo(
    const std::filesystem::path& videoPath);

}  // namespace playback_video_transcript
