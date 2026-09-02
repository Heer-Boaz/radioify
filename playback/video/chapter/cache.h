#pragma once

#include <optional>
#include <string>

#include "playback/video/chapter/backend.h"

namespace playback_video_chapters {

// Stable identity shared by durable publication and resumable private work.
// It includes source metadata, selected stream, transcript evidence, model
// hashes, and the cache schema.
std::string analysisSourceKey(const AnalysisRequest& request);

std::optional<AnalysisResult> loadCachedAnalysis(
    const AnalysisRequest& request);
bool storeCachedAnalysis(const AnalysisRequest& request,
                         const AnalysisResult& result,
                         std::string* error = nullptr);

}  // namespace playback_video_chapters
