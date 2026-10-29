#pragma once

#include "playback/video/transcript/transcriber.h"

namespace playback_video_transcript {

enum class RuntimeComponent { Quality, NativeWhisper };
struct TranscriptRuntime {
  std::filesystem::path directory;
  std::filesystem::path worker;
};

// Called only by an explicit transcription/review operation. Downloads stay in
// writable user data, independently of the installed application's lifetime.
bool ensureTranscriptRuntime(RuntimeComponent component,
                             const ProgressCallback &progress,
                             const std::function<bool()> &cancelled,
                             TranscriptRuntime *runtime, std::string *error);

} // namespace playback_video_transcript
