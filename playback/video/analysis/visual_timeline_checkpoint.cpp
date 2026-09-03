#include "playback/video/analysis/visual_timeline_scan.h"

#include <algorithm>
#include <cmath>

namespace playback_video_analysis {

bool validVisualTimelineScanCheckpoint(
    const VisualTimelineScanRequest &request,
    const VisualTimelineScanCheckpoint &checkpoint) {
  if (!checkpoint.initialized) {
    return checkpoint.videoPath.empty() && checkpoint.videoStreamIndex == -1 &&
           checkpoint.expectedDurationUs == 0 && !checkpoint.requireD3d11 &&
           checkpoint.durationUs == 0 && checkpoint.nextSampleUs == 0 &&
           checkpoint.samples.empty();
  }
  if (checkpoint.videoPath != request.videoPath ||
      checkpoint.videoStreamIndex != request.videoStreamIndex ||
      checkpoint.expectedDurationUs != request.expectedDurationUs ||
      checkpoint.requireD3d11 != request.requireD3d11 ||
      checkpoint.durationUs <= 0 || checkpoint.nextSampleUs < 0 ||
      checkpoint.nextSampleUs % kVisualSampleIntervalUs != 0 ||
      (request.expectedDurationUs > 0 &&
       checkpoint.durationUs != request.expectedDurationUs)) {
    return false;
  }
  const std::size_t maximumSamples =
      static_cast<std::size_t>(std::min<std::int64_t>(
          checkpoint.durationUs / kVisualSampleIntervalUs + 2, 100'000));
  if (checkpoint.samples.size() > maximumSamples)
    return false;
  std::int64_t previousTimestampUs = -1;
  for (const VisualSample &sample : checkpoint.samples) {
    if (sample.timestampUs < 0 || sample.timestampUs <= previousTimestampUs ||
        sample.timestampUs > checkpoint.durationUs ||
        !std::isfinite(sample.meanLuma) ||
        !std::isfinite(sample.darkFraction) ||
        !std::isfinite(sample.changeScore) ||
        !std::isfinite(sample.letterboxConfidence) ||
        !std::isfinite(sample.borderEdgeDensity) ||
        !std::isfinite(sample.centerEdgeDensity)) {
      return false;
    }
    previousTimestampUs = sample.timestampUs;
  }
  return checkpoint.samples.empty()
             ? checkpoint.nextSampleUs == 0
             : checkpoint.nextSampleUs > previousTimestampUs;
}

} // namespace playback_video_analysis
