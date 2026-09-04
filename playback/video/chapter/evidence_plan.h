#pragma once

#include <cstdint>
#include <vector>

namespace playback_video_chapters {

// Builds Chapter-Llama's published speech-guided frame schedule from the
// ASR-only planner's chapter predictions. It preserves every candidate except
// for Chapter-Llama's published one-second opening-frame normalization; it
// never inserts locally shifted or periodic fallback samples.
std::vector<std::int64_t> buildSpeechGuidedFrameSchedule(
    std::int64_t durationUs,
    const std::vector<std::int64_t> &predictedChapterStartsUs);

} // namespace playback_video_chapters
