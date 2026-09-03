#pragma once

#include <filesystem>
#include <string>

#include "playback/video/chapter/inference.h"

namespace playback_video_chapters::inference_worker_protocol {

// Schema 5 invalidates checkpoints produced before the guarded planner prompt
// and aggregate caption-context admission contract.
inline constexpr int kSchema = 5;

// Serializes the parent-owned request before the helper process starts. The
// matching reader and all schema details remain private to the worker target.
bool storeRequest(const std::filesystem::path &workspace,
                  const InferenceRequest &request, std::string *error);

} // namespace playback_video_chapters::inference_worker_protocol
