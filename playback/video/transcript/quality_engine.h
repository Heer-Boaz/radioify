#pragma once

#include "playback/video/transcript/transcriber.h"

namespace playback_video_transcript {

// Manual transcripts use Qwen for recognition and Whisper for alignment. The
// resumable Whisper operation remains available for English translation.
bool createQualityTranscript(
    const std::filesystem::path &videoPath,
    const std::filesystem::path &outputPath, TranscriptPublishMode publishMode,
    const ProgressCallback &onProgress,
    const std::atomic<bool> *cancelRequested, std::string *error,
    const TranscriptCommitStarted &outputCommitStarted,
    std::filesystem::path *publishedPathResult);

} // namespace playback_video_transcript
