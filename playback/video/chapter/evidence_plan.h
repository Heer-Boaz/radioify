#pragma once

#include <cstdint>
#include <vector>

#include "playback/video/analysis/scene_analysis.h"

namespace playback_video_chapters {

struct ChapterEvidenceInterval {
  std::int64_t startUs = 0;
  std::int64_t endUs = 0;
  std::vector<std::int64_t> sampleTimesUs;
};

// Turns the complete dense timeline scan into uniformly spaced temporal work
// units. Each unit contains a chronological image sequence rather than an
// isolated thumbnail. The ten-second evidence cadence follows Chapter-Llama's
// published no-ASR fallback; grouping adjacent samples into one bounded window
// lets the VLM observe activity over time without putting an hour-long video
// into one inference context.
std::vector<ChapterEvidenceInterval> buildChapterEvidencePlan(
    std::int64_t durationUs,
    const std::vector<playback_video_analysis::VisualSample> &samples);

} // namespace playback_video_chapters
