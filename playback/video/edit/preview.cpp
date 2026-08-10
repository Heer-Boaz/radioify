#include "playback/video/edit/preview.h"

#include <algorithm>
#include <limits>

namespace playback_video_edit {
namespace {

bool validRanges(const std::vector<SourceRange>& ranges) {
  if (ranges.empty()) return false;
  int64_t previousEnd = -1;
  for (const SourceRange& range : ranges) {
    if (range.startUs < 0 || range.endUs <= range.startUs ||
        range.startUs < previousEnd) {
      return false;
    }
    previousEnd = range.endUs;
  }
  return true;
}

int64_t frameEndUs(int64_t positionUs, int64_t durationUs) {
  const int64_t duration = std::max<int64_t>(1, durationUs);
  if (positionUs > (std::numeric_limits<int64_t>::max)() - duration) {
    return (std::numeric_limits<int64_t>::max)();
  }
  return positionUs + duration;
}

}  // namespace

bool PreviewController::start(const std::vector<SourceRange>& ranges,
                              int64_t* firstTargetUs) {
  stop();
  if (!validRanges(ranges)) return false;
  ranges_ = ranges;
  rangeIndex_ = 0;
  active_ = true;
  if (firstTargetUs) *firstTargetUs = ranges_.front().startUs;
  return true;
}

void PreviewController::stop() {
  active_ = false;
  rangeIndex_ = 0;
  ranges_.clear();
}

PreviewDecision PreviewController::observePresentedFrame(
    int64_t positionUs, int64_t frameDurationUs, bool seekPending) {
  if (!active_ || seekPending || rangeIndex_ >= ranges_.size()) return {};
  const SourceRange& current = ranges_[rangeIndex_];
  if (positionUs < current.startUs || positionUs >= current.endUs) {
    return {PreviewDecisionKind::Seek, current.startUs};
  }
  if (frameEndUs(positionUs, frameDurationUs) < current.endUs) return {};
  ++rangeIndex_;
  if (rangeIndex_ >= ranges_.size()) {
    active_ = false;
    return {PreviewDecisionKind::Complete, 0};
  }
  return {PreviewDecisionKind::Seek, ranges_[rangeIndex_].startUs};
}

}  // namespace playback_video_edit
