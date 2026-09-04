#pragma once

#include <cstdint>
#include <vector>

namespace playback_video_chapters {

struct ChapterEvidenceInterval {
  std::int64_t startUs = 0;
  std::int64_t endUs = 0;
  std::vector<std::int64_t> sampleTimesUs;
};

// Builds Chapter-Llama's published speech-guided frame schedule from the
// ASR-only planner's chapter predictions. The first boundary at video start is
// sampled at one second to avoid the common black opening frame, matching the
// reference ASR-first sampling script.
std::vector<ChapterEvidenceInterval> buildSpeechGuidedChapterEvidencePlan(
    std::int64_t durationUs,
    const std::vector<std::int64_t> &predictedChapterStartsUs);

} // namespace playback_video_chapters
