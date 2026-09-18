#pragma once

#include <filesystem>
#include <string>

#include "playback/video/analysis/inference.h"
#include "playback/video/transcript/worker_types.h"

namespace playback_video_analysis::inference_worker_protocol {

bool storeVideoReviewRequest(
    const std::filesystem::path &workspace,
    const playback_video_analysis::ReviewRequest &request, std::string *error);
bool storeSpeechTranscriptionRequest(
    const std::filesystem::path &workspace,
    const playback_video_transcript::SpeechWorkerRequest &request,
    std::string *error);

} // namespace playback_video_analysis::inference_worker_protocol
