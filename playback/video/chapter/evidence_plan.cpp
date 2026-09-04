#include "playback/video/chapter/evidence_plan.h"

#include <algorithm>

#include "playback/video/chapter/chapter.h"

namespace playback_video_chapters {
std::vector<std::int64_t> buildSpeechGuidedFrameSchedule(
    std::int64_t durationUs,
    const std::vector<std::int64_t> &predictedChapterStartsUs) {
  if (durationUs < kMinimumAutomaticChapterVideoDurationUs ||
      durationUs > kMaximumAutomaticChapterVideoDurationUs ||
      predictedChapterStartsUs.size() < kMinimumAutomaticChapterCount ||
      predictedChapterStartsUs.size() > kMaximumAutomaticChapterCount ||
      predictedChapterStartsUs.front() != 0 ||
      !std::is_sorted(predictedChapterStartsUs.begin(),
                      predictedChapterStartsUs.end()) ||
      std::adjacent_find(predictedChapterStartsUs.begin(),
                         predictedChapterStartsUs.end()) !=
          predictedChapterStartsUs.end() ||
      predictedChapterStartsUs.back() >= durationUs) {
    return {};
  }

  std::vector<std::int64_t> sampleTimesUs = predictedChapterStartsUs;
  // Chapter-Llama's published caption-selection pipeline avoids the often
  // blank frame at timestamp zero by moving only that opening sample to one
  // second. This is part of the model's evidence contract, not a periodic or
  // locally inferred fallback.
  sampleTimesUs.front() = (std::min)(std::int64_t{1'000'000}, durationUs - 1);
  return sampleTimesUs;
}

} // namespace playback_video_chapters
