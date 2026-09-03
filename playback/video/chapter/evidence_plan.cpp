#include "playback/video/chapter/evidence_plan.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "playback/video/chapter/chapter.h"

namespace playback_video_chapters {
namespace {

using playback_video_analysis::VisualSample;

// Chapter-Llama uses a frame every ten seconds when speech-guided selection is
// unavailable. Radioify retains that published evidence cadence, while six
// adjacent samples form one bounded multi-image observation for the VLM.
constexpr std::int64_t kEvidenceSampleIntervalUs = 10'000'000;
constexpr std::size_t kSamplesPerTemporalWindow = 6;
constexpr std::int64_t kPreferredTemporalWindowUs =
    kEvidenceSampleIntervalUs * kSamplesPerTemporalWindow;

std::size_t evidenceIntervalCount(std::int64_t durationUs) {
  if (durationUs < kMinimumAutomaticChapterVideoDurationUs ||
      durationUs > kMaximumAutomaticChapterVideoDurationUs)
    return 0;
  const std::int64_t durationLimitedCount =
      durationUs / kMinimumAutomaticChapterDurationUs;
  const std::int64_t preferredCount =
      std::max<std::int64_t>(1, (durationUs + kPreferredTemporalWindowUs - 1) /
                                    kPreferredTemporalWindowUs);
  return static_cast<std::size_t>(std::clamp<std::int64_t>(
      std::min(preferredCount, durationLimitedCount),
      static_cast<std::int64_t>(kMinimumAutomaticEvidenceSampleCount),
      static_cast<std::int64_t>(kMaximumAutomaticEvidenceSampleCount)));
}

std::int64_t partitionBoundary(std::int64_t durationUs, std::size_t index,
                               std::size_t count) {
  // Divide before multiplying so even multi-hour media cannot overflow.
  const std::int64_t quotient = durationUs / static_cast<std::int64_t>(count);
  const std::int64_t remainder = durationUs % static_cast<std::int64_t>(count);
  return quotient * static_cast<std::int64_t>(index) +
         (remainder * static_cast<std::int64_t>(index)) /
             static_cast<std::int64_t>(count);
}

std::int64_t representativeTime(const std::vector<VisualSample> &samples,
                                std::int64_t targetUs, std::int64_t firstUs,
                                std::int64_t afterLastUs) {
  if (firstUs >= afterLastUs)
    return firstUs;
  const std::int64_t marginUs =
      std::min<std::int64_t>(1'000'000, (afterLastUs - firstUs) / 5);
  const std::int64_t candidateFirstUs = firstUs + marginUs;
  const std::int64_t candidateLastUs =
      std::max(candidateFirstUs, afterLastUs - marginUs - 1);
  const double halfWidth =
      std::max<double>(1.0, static_cast<double>(afterLastUs - firstUs) * 0.5);

  const VisualSample *best = nullptr;
  double bestRank = -(std::numeric_limits<double>::infinity)();
  for (const VisualSample &sample : samples) {
    if (sample.timestampUs < candidateFirstUs)
      continue;
    if (sample.timestampUs > candidateLastUs)
      break;
    const double distance =
        static_cast<double>(std::abs(sample.timestampUs - targetUs)) /
        halfWidth;
    const double detail = std::min<double>(1.0, sample.centerEdgeDensity * 4.0);
    // Prefer a readable frame near the interval centre. Darkness and a frame
    // directly on a cut are generic evidence-quality penalties, not semantic
    // labels or hand-authored content rules.
    const double rank = (1.0 - sample.darkFraction) * 0.45 + detail * 0.20 -
                        sample.changeScore * 0.20 - distance * 0.35;
    if (!best || rank > bestRank) {
      best = &sample;
      bestRank = rank;
    }
  }
  return best ? best->timestampUs
              : std::clamp(targetUs, firstUs, afterLastUs - 1);
}

} // namespace

std::vector<ChapterEvidenceInterval>
buildChapterEvidencePlan(std::int64_t durationUs,
                         const std::vector<VisualSample> &samples) {
  const std::size_t count = evidenceIntervalCount(durationUs);
  if (count == 0 || samples.size() < 2)
    return {};

  std::vector<ChapterEvidenceInterval> intervals;
  intervals.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    ChapterEvidenceInterval interval;
    interval.startUs = partitionBoundary(durationUs, index, count);
    interval.endUs = partitionBoundary(durationUs, index + 1, count);
    if (interval.endUs - interval.startUs <
        kMinimumAutomaticChapterDurationUs) {
      return {};
    }
    const std::int64_t windowDurationUs = interval.endUs - interval.startUs;
    const std::size_t sampleCount =
        static_cast<std::size_t>(std::clamp<std::int64_t>(
            (windowDurationUs + kEvidenceSampleIntervalUs - 1) /
                kEvidenceSampleIntervalUs,
            1, static_cast<std::int64_t>(kSamplesPerTemporalWindow)));
    interval.sampleTimesUs.reserve(sampleCount);
    for (std::size_t sampleIndex = 0; sampleIndex < sampleCount;
         ++sampleIndex) {
      const std::int64_t cellStartUs =
          partitionBoundary(windowDurationUs, sampleIndex, sampleCount) +
          interval.startUs;
      const std::int64_t cellEndUs =
          partitionBoundary(windowDurationUs, sampleIndex + 1, sampleCount) +
          interval.startUs;
      const std::int64_t targetUs = cellStartUs + (cellEndUs - cellStartUs) / 2;
      interval.sampleTimesUs.push_back(
          representativeTime(samples, targetUs, cellStartUs, cellEndUs));
    }
    intervals.push_back(interval);
  }
  return intervals;
}

} // namespace playback_video_chapters
