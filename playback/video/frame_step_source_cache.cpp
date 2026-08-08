#include "playback/video/frame_step_source_cache.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace playback_video_frame_step_prefetch {
namespace {

int64_t frameEndUs(const SourceFrame& frame) {
  if (frame.durationUs <= 0 ||
      frame.sourcePtsUs >
          (std::numeric_limits<int64_t>::max)() - frame.durationUs) {
    return frame.sourcePtsUs;
  }
  return frame.sourcePtsUs + frame.durationUs;
}

bool frameLess(const std::shared_ptr<const SourceFrame>& left,
               const std::shared_ptr<const SourceFrame>& right) {
  if (left->sourcePtsUs != right->sourcePtsUs) {
    return left->sourcePtsUs < right->sourcePtsUs;
  }
  if (left->identity.sourcePtsTicks != right->identity.sourcePtsTicks) {
    return left->identity.sourcePtsTicks < right->identity.sourcePtsTicks;
  }
  return left->identity.sourceDtsTicks < right->identity.sourceDtsTicks;
}

}  // namespace

bool SourceFrameCache::insert(std::shared_ptr<const SourceFrame> frame) {
  return insertBatch({std::move(frame)});
}

bool SourceFrameCache::insertBatch(
    const std::vector<std::shared_ptr<const SourceFrame>>& frames) {
  if (frames.empty()) {
    return true;
  }

  std::vector<std::shared_ptr<const SourceFrame>> committed = frames_;
  committed.reserve(committed.size() + frames.size());
  for (const auto& frame : frames) {
    if (!frame || frame->sourcePtsUs < 0 || frame->durationUs <= 0 ||
        std::any_of(committed.begin(), committed.end(),
                    [&](const auto& candidate) {
                      return sameIdentity(candidate->identity,
                                          frame->identity);
                    })) {
      return false;
    }
    auto position =
        std::lower_bound(committed.begin(), committed.end(), frame, frameLess);
    committed.insert(position, frame);
  }
  frames_.swap(committed);
  return true;
}

std::shared_ptr<const SourceFrame> SourceFrameCache::find(
    const FrameIdentity& identity) const {
  auto found = std::find_if(
      frames_.begin(), frames_.end(), [&](const auto& candidate) {
        return sameIdentity(candidate->identity, identity);
      });
  return found == frames_.end() ? std::shared_ptr<const SourceFrame>{}
                                : *found;
}

std::vector<std::shared_ptr<const SourceFrame>>
SourceFrameCache::framesInRange(int64_t startUs, int64_t endUs) const {
  std::vector<std::shared_ptr<const SourceFrame>> result;
  if (startUs < 0 || endUs <= startUs) {
    return result;
  }
  for (const auto& frame : frames_) {
    if (frame->sourcePtsUs >= endUs) {
      break;
    }
    if (frameEndUs(*frame) > startUs) {
      result.push_back(frame);
    }
  }
  return result;
}

void SourceFrameCache::addCoverage(int64_t startUs, int64_t endUs) {
  CoverageSpan incoming{startUs, endUs};
  if (!incoming.valid()) {
    return;
  }
  std::vector<CoverageSpan> merged;
  merged.reserve(coverage_.size() + 1);
  bool inserted = false;
  for (const CoverageSpan& span : coverage_) {
    if (span.endUs < incoming.startUs) {
      merged.push_back(span);
    } else if (incoming.endUs < span.startUs) {
      if (!inserted) {
        merged.push_back(incoming);
        inserted = true;
      }
      merged.push_back(span);
    } else {
      incoming.startUs = (std::min)(incoming.startUs, span.startUs);
      incoming.endUs = (std::max)(incoming.endUs, span.endUs);
    }
  }
  if (!inserted) {
    merged.push_back(incoming);
  }
  coverage_ = std::move(merged);
}

bool SourceFrameCache::covers(int64_t startUs, int64_t endUs) const {
  return std::any_of(coverage_.begin(), coverage_.end(),
                     [&](const CoverageSpan& span) {
                       return span.covers(startUs, endUs);
                     });
}

std::optional<CoverageSpan> SourceFrameCache::bestCoverageFor(
    int64_t startUs, int64_t endUs) const {
  std::optional<CoverageSpan> best;
  int64_t bestOverlap = 0;
  for (const CoverageSpan& span : coverage_) {
    const int64_t overlapStart = (std::max)(startUs, span.startUs);
    const int64_t overlapEnd = (std::min)(endUs, span.endUs);
    const int64_t overlap = (std::max)(int64_t{0}, overlapEnd - overlapStart);
    if (overlap > bestOverlap) {
      best = span;
      bestOverlap = overlap;
    }
  }
  return best;
}

void SourceFrameCache::retainRange(int64_t startUs, int64_t endUs) {
  if (startUs < 0 || endUs <= startUs) {
    clear();
    return;
  }
  frames_.erase(
      std::remove_if(frames_.begin(), frames_.end(), [&](const auto& frame) {
        return frameEndUs(*frame) <= startUs || frame->sourcePtsUs >= endUs;
      }),
      frames_.end());

  std::vector<CoverageSpan> retainedCoverage;
  retainedCoverage.reserve(coverage_.size());
  for (const CoverageSpan& span : coverage_) {
    CoverageSpan retained{(std::max)(startUs, span.startUs),
                          (std::min)(endUs, span.endUs)};
    if (retained.valid()) {
      retainedCoverage.push_back(retained);
    }
  }
  coverage_ = std::move(retainedCoverage);
}

void SourceFrameCache::clear() {
  frames_.clear();
  coverage_.clear();
}

const std::shared_ptr<const SourceFrame>& SourceFrameCache::first() const {
  assert(!frames_.empty());
  return frames_.front();
}

const std::shared_ptr<const SourceFrame>& SourceFrameCache::last() const {
  assert(!frames_.empty());
  return frames_.back();
}

DecodePlan planDecode(const SourceFrameCache& cache, int64_t rangeStartUs,
                      int64_t rangeEndUs,
                      const std::optional<FrameIdentity>& decoderTail) {
  DecodePlan plan;
  plan.decodeStartUs = rangeStartUs;
  plan.decodeEndUs = rangeEndUs;
  if (rangeStartUs < 0 || rangeEndUs <= rangeStartUs) {
    return plan;
  }
  if (cache.covers(rangeStartUs, rangeEndUs)) {
    plan.kind = DecodePlanKind::CacheHit;
    return plan;
  }

  const std::optional<CoverageSpan> coverage =
      cache.bestCoverageFor(rangeStartUs, rangeEndUs);
  if (!coverage || cache.empty()) {
    plan.kind = DecodePlanKind::SeekForward;
    return plan;
  }

  if (coverage->startUs > rangeStartUs &&
      coverage->endUs >= rangeEndUs) {
    plan.kind = DecodePlanKind::SeekForward;
    plan.decodeEndUs = coverage->startUs;
    const auto joinFrames =
        cache.framesInRange(coverage->startUs, coverage->endUs);
    if (!joinFrames.empty()) {
      plan.stopAtIdentity = joinFrames.front()->identity;
    }
    return plan;
  }

  if (coverage->startUs <= rangeStartUs &&
      coverage->endUs < rangeEndUs) {
    const auto coveredFrames =
        cache.framesInRange(rangeStartUs, coverage->endUs);
    const std::shared_ptr<const SourceFrame> tailFrame =
        decoderTail ? cache.find(*decoderTail)
                    : std::shared_ptr<const SourceFrame>{};
    if (tailFrame && tailFrame->sourcePtsUs < coverage->endUs &&
        frameEndUs(*tailFrame) > coverage->startUs) {
      plan.kind = DecodePlanKind::ContinueForward;
      plan.decodeStartUs = coverage->endUs;
      return plan;
    }
    plan.kind = DecodePlanKind::SeekForward;
    if (!coveredFrames.empty()) {
      plan.decodeStartUs = coveredFrames.back()->sourcePtsUs;
    } else {
      plan.decodeStartUs = coverage->endUs;
    }
    return plan;
  }

  plan.kind = DecodePlanKind::SeekForward;
  return plan;
}

}  // namespace playback_video_frame_step_prefetch
