#pragma once

#include <optional>

#include "playback/video/chapter/backend.h"

namespace playback_video_chapters {

std::optional<AnalysisResult> loadCachedAnalysis(
    const AnalysisRequest& request);
bool storeCachedAnalysis(const AnalysisRequest& request,
                         const AnalysisResult& result,
                         std::string* error = nullptr);

}  // namespace playback_video_chapters
