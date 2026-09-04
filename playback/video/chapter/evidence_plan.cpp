#include "playback/video/chapter/evidence_plan.h"

#include <algorithm>

#include "playback/video/chapter/chapter.h"

namespace playback_video_chapters {
namespace {

std::vector<ChapterEvidenceInterval>
intervalsFromSampleTimes(std::int64_t durationUs,
                         const std::vector<std::int64_t> &sampleTimes) {
  if (sampleTimes.size() < kMinimumAutomaticEvidenceSampleCount ||
      sampleTimes.size() > kMaximumAutomaticEvidenceSampleCount ||
      !std::is_sorted(sampleTimes.begin(), sampleTimes.end()) ||
      std::adjacent_find(sampleTimes.begin(), sampleTimes.end()) !=
          sampleTimes.end() ||
      sampleTimes.front() < 0 || sampleTimes.back() >= durationUs) {
    return {};
  }

  std::vector<ChapterEvidenceInterval> intervals;
  intervals.reserve(sampleTimes.size());
  for (std::size_t index = 0; index < sampleTimes.size(); ++index) {
    ChapterEvidenceInterval interval;
    interval.startUs =
        index == 0
            ? 0
            : sampleTimes[index - 1] +
                  (sampleTimes[index] - sampleTimes[index - 1]) / 2;
    interval.endUs =
        index + 1 == sampleTimes.size()
            ? durationUs
            : sampleTimes[index] +
                  (sampleTimes[index + 1] - sampleTimes[index]) / 2;
    interval.sampleTimesUs.push_back(sampleTimes[index]);
    intervals.push_back(std::move(interval));
  }
  return intervals;
}

} // namespace

std::vector<ChapterEvidenceInterval> buildSpeechGuidedChapterEvidencePlan(
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

  std::vector<std::int64_t> sampleTimes = predictedChapterStartsUs;
  sampleTimes.front() = std::min<std::int64_t>(1'000'000, durationUs - 1);
  return intervalsFromSampleTimes(durationUs, sampleTimes);
}

} // namespace playback_video_chapters
