#pragma once

#include <filesystem>
#include <string_view>

namespace playback_video_transcript {

// Radioify owns one active transcript artifact next to each video. New
// transcripts are always published to this canonical path.
std::filesystem::path
transcriptPathForVideo(const std::filesystem::path &videoPath);

// New transcripts persist their detected language in the sidecar name so
// downstream consumers never have to infer language from dialogue text.
std::filesystem::path
languageTaggedTranscriptPathForVideo(const std::filesystem::path &videoPath,
                                     std::string_view language);

// Resolves the transcript consumed by playback and analysis. The canonical
// artifact wins whenever it exists. Numbered files from older Radioify builds
// are considered only as a migration fallback, newest first.
std::filesystem::path
activeTranscriptPathForVideo(const std::filesystem::path &videoPath);

} // namespace playback_video_transcript
