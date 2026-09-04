#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

#include "playback/video/transcript/document.h"

namespace playback_video_transcript {

struct Progress {
  float fraction = 0.0f;
  std::string phase;
};

using ProgressCallback = std::function<void(const Progress &)>;
using TranscriptCommitStarted = std::function<bool()>;

// Decodes the video's primary audio stream to Whisper's native mono 16 kHz
// format and writes a timestamp-indexed SRT sidecar. The destination is not
// changed unless the complete transcript succeeds.
bool createIndexedTranscript(
    const std::filesystem::path &videoPath,
    const std::filesystem::path &outputPath, TranscriptPublishMode publishMode,
    const ProgressCallback &onProgress,
    const std::atomic<bool> *cancelRequested, std::string *error,
    const TranscriptCommitStarted &outputCommitStarted = {},
    std::filesystem::path *publishedPath = nullptr);

} // namespace playback_video_transcript
