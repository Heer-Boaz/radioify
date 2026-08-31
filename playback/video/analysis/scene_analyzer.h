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

// Scene analysis runs inside an active interactive video session. Keep its
// sparse background decoder on the CPU so the foreground D3D11 decoder and
// renderer remain the sole GPU owner.
inline constexpr bool kSceneAnalysisPreferHardwareDecode = false;

// Decodes a sparse, downscaled visual stream and combines it with bounded
// speech activity from Radioify's newest indexed transcript. Decoding and
// classification deliberately stay on the low-priority CPU worker.
bool analyzeVideoScenes(const std::filesystem::path& videoPath,
                        int videoStreamIndex, int64_t expectedDurationUs,
                        const AnalysisProgressCallback& onProgress,
                        const std::atomic<bool>* cancelRequested,
                        bool allowCachedResult,
                        AnalysisResult* result, std::string* error);

}  // namespace playback_video_analysis
