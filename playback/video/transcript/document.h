#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace playback_video_transcript {

struct Segment {
  int64_t startUs = 0;
  int64_t endUs = 0;
  std::string text;
};

// Rejects empty/model-placeholder cues before they can affect later chunk
// prompts or reach the final sidecar.
bool isMeaningfulTranscriptText(const std::string& text);

std::filesystem::path defaultTranscriptPath(
    const std::filesystem::path& videoPath);
std::filesystem::path availableTranscriptPath(
    const std::filesystem::path& videoPath);

// Writes a numbered, time-indexed SRT sidecar without exposing a partially
// written destination. Existing files are never overwritten.
bool writeIndexedTranscript(const std::filesystem::path& outputPath,
                            const std::vector<Segment>& segments,
                            std::string* error);

}  // namespace playback_video_transcript
