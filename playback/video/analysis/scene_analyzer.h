#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "playback/video/analysis/scene_analysis.h"

namespace playback_video_analysis {

struct AnalysisProgress {
  double fraction = 0.0;
  std::string phase;
};

using AnalysisProgressCallback =
    std::function<void(const AnalysisProgress&)>;

struct AnalysisResult {
  int64_t durationUs = 0;
  size_t visualSampleCount = 0;
  std::filesystem::path transcriptPath;
  std::vector<SceneSuggestion> suggestions;
};

// Scene analysis can scan long sources, so decoding throughput is part of the
// product contract even though sampling is sparse. Prefer hardware decode;
// the analysis job remains background-priority and cancellable.
inline constexpr bool kSceneAnalysisPreferHardwareDecode = true;

// Decodes a sparse, downscaled visual stream and combines it with bounded
// speech activity from Radioify's newest indexed transcript. Video decoding
// prefers hardware acceleration; classification remains a small,
// deterministic CPU projection over the sampled signatures.
bool analyzeVideoScenes(const std::filesystem::path& videoPath,
                        int videoStreamIndex, int64_t expectedDurationUs,
                        const AnalysisProgressCallback& onProgress,
                        const std::atomic<bool>* cancelRequested,
                        bool allowCachedResult,
                        AnalysisResult* result, std::string* error);

}  // namespace playback_video_analysis
