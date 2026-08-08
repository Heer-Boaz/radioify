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

void HoverModel::start(int64_t durationUs) {
  snapshot_ = Snapshot{};
  snapshot_.durationUs = std::max<int64_t>(0, durationUs);
  requestId_ = 0;
  activeDecodeTargetUs_ = -1;
  activeBucketUs_ = 0;
  lastRequestedDecodeTargetUs_ = -1;
}

void HoverModel::stop() {
  snapshot_ = Snapshot{};
  requestId_ = 0;
  activeDecodeTargetUs_ = -1;
  activeBucketUs_ = 0;
  lastRequestedDecodeTargetUs_ = -1;
}

uint64_t HoverModel::nextRequestId() {
  ++requestId_;
  if (requestId_ == 0) ++requestId_;
  return requestId_;
}

HoverModel::Update HoverModel::hover(double ratio, int progressUnits) {
  Update update;
  if (!std::isfinite(ratio)) return update;

  if (snapshot_.durationUs <= 0) return update;

  ratio = std::clamp(ratio, 0.0, 1.0);
  const int64_t targetUs = targetForRatio(ratio, snapshot_.durationUs);
  const int64_t bucketUs =
      bucketDurationUs(snapshot_.durationUs, progressUnits);
  const int64_t decodeTargetUs =
      bucketTargetUs(targetUs, snapshot_.durationUs, bucketUs);
  const bool sameTarget =
      snapshot_.visible && activeDecodeTargetUs_ == decodeTargetUs &&
      activeBucketUs_ == bucketUs;

  snapshot_.visible = true;
  snapshot_.anchorRatio = ratio;
  snapshot_.targetUs = targetUs;
  snapshot_.failed = sameTarget ? snapshot_.failed : false;
  ++snapshot_.revision;
  update.changed = true;
  if (sameTarget) return update;

  const int direction =
      lastRequestedDecodeTargetUs_ < 0
          ? 0
          : (decodeTargetUs > lastRequestedDecodeTargetUs_
                 ? 1
                 : (decodeTargetUs < lastRequestedDecodeTargetUs_ ? -1 : 0));
  lastRequestedDecodeTargetUs_ = decodeTargetUs;
  activeDecodeTargetUs_ = decodeTargetUs;
  activeBucketUs_ = bucketUs;
  snapshot_.loading = true;
  snapshot_.failed = false;
  snapshot_.image.reset();

  Request request;
  request.id = nextRequestId();
  request.targetUs = decodeTargetUs;
  request.prefetchTargetsUs =
      prefetchTargets(decodeTargetUs, bucketUs, snapshot_.durationUs,
                      direction);
  update.request = std::move(request);
  return update;
}

bool HoverModel::hide() {
  if (!snapshot_.visible) return false;
  snapshot_.visible = false;
  snapshot_.loading = false;
  snapshot_.failed = false;
  snapshot_.image.reset();
  activeDecodeTargetUs_ = -1;
  activeBucketUs_ = 0;
  lastRequestedDecodeTargetUs_ = -1;
  nextRequestId();
  ++snapshot_.revision;
  return true;
}

bool HoverModel::reject(const Request& request) {
  if (!snapshot_.visible || request.id != requestId_ ||
      request.targetUs != activeDecodeTargetUs_) {
    return false;
  }
  snapshot_.image.reset();
  snapshot_.loading = false;
  snapshot_.failed = true;
  nextRequestId();
  ++snapshot_.revision;
  return true;
}

bool HoverModel::apply(const Result& result) {
  if (!snapshot_.visible || result.requestId != requestId_ ||
      result.targetUs != activeDecodeTargetUs_) {
    return false;
  }
  if (result.image && result.image->requestedUs != activeDecodeTargetUs_) {
    return false;
  }
  snapshot_.image = result.image;
  snapshot_.loading = false;
  snapshot_.failed = !result.image;
  ++snapshot_.revision;
  return true;
}

uint64_t HoverModel::requestId() const {
  return requestId_;
}

Snapshot HoverModel::snapshot() const { return snapshot_; }

}  // namespace playback_video_timeline_preview
