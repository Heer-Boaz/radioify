#pragma once

#include <cstdint>
#include <filesystem>

#include "playback/video/analysis/scene_analyzer.h"

namespace playback_video_analysis {

// Cache identity includes the source file, selected stream, analyzer schema,
// and newest transcript sidecar. Corrupt or stale entries are ordinary cache
// misses and never block a fresh analysis.
bool loadCachedSceneAnalysis(const std::filesystem::path& videoPath,
                             int videoStreamIndex, int64_t durationUs,
                             const std::filesystem::path& transcriptPath,
                             AnalysisResult* result);

void storeCachedSceneAnalysis(const std::filesystem::path& videoPath,
                              int videoStreamIndex, int64_t durationUs,
                              const std::filesystem::path& transcriptPath,
                              const AnalysisResult& result);

}  // namespace playback_video_analysis
