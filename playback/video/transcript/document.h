#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "playback/video/transcript/cue.h"

namespace playback_video_transcript {

std::filesystem::path defaultTranscriptPath(
    const std::filesystem::path& videoPath);
std::filesystem::path availableTranscriptPath(
    const std::filesystem::path& videoPath);

// Returns the newest Radioify-owned transcript sidecar for a video. This is
// deliberately separate from availableTranscriptPath(): readers select an
// existing immutable result, while writers reserve a new destination.
std::filesystem::path latestIndexedTranscriptPath(
    const std::filesystem::path& videoPath);

// Reads ordinary SRT timing/text into the transcript domain. Keeping this
// parser here prevents downstream analysis from treating subtitle
// presentation spans as its own storage format.
bool readIndexedTranscript(const std::filesystem::path& inputPath,
                           std::vector<Segment>* segments,
                           std::string* error);

// Writes a numbered, time-indexed SRT sidecar without exposing a partially
// written destination. Existing files are never overwritten.
bool writeIndexedTranscript(const std::filesystem::path& outputPath,
                            const std::vector<Segment>& segments,
                            std::string* error);

}  // namespace playback_video_transcript
