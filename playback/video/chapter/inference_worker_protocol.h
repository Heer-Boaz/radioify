#pragma once

#include <filesystem>
#include <string>

#include "playback/video/chapter/inference.h"
#include "playback/video/transcript/worker_types.h"

namespace playback_video_chapters::inference_worker_protocol {

// Schema 15 carries one exact frame for every ASR-selected candidate instead
// of the obsolete temporal-window envelope, plus killable Whisper inference.
inline constexpr int kSchema = 15;

// Serializes the parent-owned request before the helper process starts. The
// matching reader and all schema details remain private to the worker target.
bool storeRequest(const std::filesystem::path &workspace,
                  const InferenceRequest &request, std::string *error);
bool storeSpeechChapterPlanRequest(
    const std::filesystem::path &workspace,
    const SpeechChapterPlanRequest &request, std::string *error);
bool storeSpeechTranscriptionRequest(
    const std::filesystem::path &workspace,
    const playback_video_transcript::SpeechWorkerRequest &request,
    std::string *error);

} // namespace playback_video_chapters::inference_worker_protocol
