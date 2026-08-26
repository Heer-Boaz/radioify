#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "playback/video/transcript/cue.h"

namespace playback_video_transcript {

enum class TranscriptPublishMode {
  CreateNew,
  ReplaceExisting,
};

// Reads ordinary SRT timing/text into the transcript domain. Keeping this
// parser here prevents downstream analysis from treating subtitle
// presentation spans as its own storage format.
bool readIndexedTranscript(const std::filesystem::path& inputPath,
                           std::vector<Segment>* segments,
                           std::string* error);

// Publishes a time-indexed SRT sidecar without exposing a partially written
// destination. ReplaceExisting atomically swaps a complete new document into
// place while preserving the old document if publication fails.
bool writeIndexedTranscript(const std::filesystem::path& outputPath,
                            const std::vector<Segment>& segments,
                            TranscriptPublishMode publishMode,
                            std::string* error);

}  // namespace playback_video_transcript
