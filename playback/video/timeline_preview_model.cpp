#include "playback/video/timeline_preview_model.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace playback_video_timeline_preview {
namespace {

int64_t targetForRatio(double ratio, int64_t durationUs) {
  if (durationUs <= 0 || !std::isfinite(ratio)) return 0;
  const int64_t lastUs = durationUs - 1;
  ratio = std::clamp(ratio, 0.0, 1.0);
  if (ratio <= 0.0) return 0;
  if (ratio >= 1.0) return lastUs;
  const double scaled = ratio * static_cast<double>(lastUs);
  if (scaled <= 0.0) return 0;
  if (scaled >= static_cast<double>(lastUs)) return lastUs;
  return std::clamp(static_cast<int64_t>(std::llround(scaled)), int64_t{0},
                    lastUs);
}

}  // namespace

void HoverModel::start(int64_t durationUs, int sourceWidth, int sourceHeight) {
  snapshot_ = Snapshot{};
  sourceDurationUs_ = std::max<int64_t>(0, durationUs);
  if (sourceDurationUs_ > 0) {
    timeline_ = playback_video_sequence::Timeline::create(
        sourceDurationUs_, {{0, sourceDurationUs_}});
  } else {
    timeline_.reset();
  }
  snapshot_.durationUs = timeline_ ? timeline_->durationUs() : 0;
  snapshot_.sourceWidth = std::max(0, sourceWidth);
  snapshot_.sourceHeight = std::max(0, sourceHeight);
  requestId_ = 0;
  activeTimelineTargetUs_ = -1;
  activeDecodeTargetUs_ = -1;
  activeBucketUs_ = 0;
  lastRequestedTimelineTargetUs_ = -1;
}

void HoverModel::stop() {
  snapshot_ = Snapshot{};
  timeline_.reset();
  sourceDurationUs_ = 0;
  requestId_ = 0;
  activeTimelineTargetUs_ = -1;
  activeDecodeTargetUs_ = -1;
  activeBucketUs_ = 0;
  lastRequestedTimelineTargetUs_ = -1;
}

bool HoverModel::setSequence(
    const std::vector<playback_video_sequence::SourceRange>& ranges) {
  const auto next = playback_video_sequence::Timeline::create(
      sourceDurationUs_, ranges);
  if (!next) return false;
  timeline_ = std::move(next);
  snapshot_.durationUs = timeline_->durationUs();
  snapshot_.hoverActive = false;
  snapshot_.image.reset();
  activeTimelineTargetUs_ = -1;
  activeDecodeTargetUs_ = -1;
  activeBucketUs_ = 0;
  lastRequestedTimelineTargetUs_ = -1;
  nextRequestId();
  ++snapshot_.revision;
  return true;
}

uint64_t HoverModel::nextRequestId() {
  ++requestId_;
  if (requestId_ == 0) ++requestId_;
  return requestId_;
}

HoverModel::Update HoverModel::hover(PresentationSurface surface, double ratio,
                                     int progressUnits) {
  Update update;
  if (!std::isfinite(ratio)) return update;

  if (snapshot_.durationUs <= 0 || !timeline_) return update;

  ratio = std::clamp(ratio, 0.0, 1.0);
  const int64_t targetUs = targetForRatio(ratio, snapshot_.durationUs);
  const int64_t bucketUs =
      bucketDurationUs(snapshot_.durationUs, progressUnits);
  const int64_t timelineDecodeTargetUs =
      bucketTargetUs(targetUs, snapshot_.durationUs, bucketUs);
  const playback_video_sequence::Point decodePoint =
      timeline_->pointAt(timelineDecodeTargetUs);
  const int64_t decodeTargetUs = std::clamp(
      decodePoint.sourceUs, int64_t{0}, sourceDurationUs_ - 1);
  const bool sameTarget =
      snapshot_.hoverActive &&
      activeTimelineTargetUs_ == timelineDecodeTargetUs &&
      activeDecodeTargetUs_ == decodeTargetUs &&
      activeBucketUs_ == bucketUs;

  snapshot_.hoverActive = true;
  snapshot_.presentationSurface = surface;
  snapshot_.anchorRatio = ratio;
  snapshot_.targetUs = targetUs;
  ++snapshot_.revision;
  update.changed = true;
  if (sameTarget) return update;

  const int direction =
      lastRequestedTimelineTargetUs_ < 0
          ? 0
          : (timelineDecodeTargetUs > lastRequestedTimelineTargetUs_
                 ? 1
                 : (timelineDecodeTargetUs < lastRequestedTimelineTargetUs_
                        ? -1
                        : 0));
  lastRequestedTimelineTargetUs_ = timelineDecodeTargetUs;
  activeTimelineTargetUs_ = timelineDecodeTargetUs;
  activeDecodeTargetUs_ = decodeTargetUs;
  activeBucketUs_ = bucketUs;
  snapshot_.image.reset();

  Request request;
  request.id = nextRequestId();
  request.targetUs = decodeTargetUs;
  const std::vector<int64_t> timelinePrefetch = prefetchTargets(
      timelineDecodeTargetUs, bucketUs, snapshot_.durationUs, direction);
  request.prefetchTargetsUs.reserve(timelinePrefetch.size());
  for (const int64_t prefetchUs : timelinePrefetch) {
    const int64_t sourceUs = std::clamp(timeline_->pointAt(prefetchUs).sourceUs,
                                        int64_t{0}, sourceDurationUs_ - 1);
    if (sourceUs == request.targetUs ||
        std::find(request.prefetchTargetsUs.begin(),
                  request.prefetchTargetsUs.end(), sourceUs) !=
            request.prefetchTargetsUs.end()) {
      continue;
    }
    request.prefetchTargetsUs.push_back(sourceUs);
  }
  update.request = std::move(request);
  return update;
}

bool HoverModel::hide(PresentationSurface surface) {
  if (!snapshot_.hoverActive || snapshot_.presentationSurface != surface) {
    return false;
  }
  snapshot_.hoverActive = false;
  snapshot_.image.reset();
  activeTimelineTargetUs_ = -1;
  activeDecodeTargetUs_ = -1;
  activeBucketUs_ = 0;
  lastRequestedTimelineTargetUs_ = -1;
  nextRequestId();
  ++snapshot_.revision;
  return true;
}

bool HoverModel::reject(const Request& request) {
  if (!snapshot_.hoverActive || request.id != requestId_ ||
      request.targetUs != activeDecodeTargetUs_) {
    return false;
  }
  snapshot_.image.reset();
  nextRequestId();
  ++snapshot_.revision;
  return true;
}

bool HoverModel::apply(const Result& result) {
  if (!snapshot_.hoverActive || result.requestId != requestId_ ||
      result.targetUs != activeDecodeTargetUs_) {
    return false;
  }
  if (result.image && result.image->requestedUs != activeDecodeTargetUs_) {
    return false;
  }
  snapshot_.image =
      result.image && playback_video_image::validate(result.image->surface)
          ? result.image
          : nullptr;
  ++snapshot_.revision;
  return true;
}

uint64_t HoverModel::requestId() const {
  return requestId_;
}

Snapshot HoverModel::snapshot() const { return snapshot_; }

Snapshot HoverModel::snapshotFor(PresentationSurface surface) const {
  Snapshot snapshot = snapshot_;
  if (snapshot.hoverActive && snapshot.presentationSurface != surface) {
    snapshot.hoverActive = false;
    snapshot.image.reset();
  }
  return snapshot;
}

}  // namespace playback_video_timeline_preview
