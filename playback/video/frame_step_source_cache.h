#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "playback/video/decoder.h"

namespace playback_video_frame_step_prefetch {

struct FrameIdentity {
  int64_t sourcePtsTicks = (std::numeric_limits<int64_t>::min)();
  int64_t sourceDtsTicks = (std::numeric_limits<int64_t>::min)();
  int64_t ptsUs = 0;
  int64_t durationUs = 0;
};

inline FrameIdentity identityFrom(const VideoReadInfo& info, int64_t ptsUs,
                                  int64_t durationUs) {
  FrameIdentity identity;
  identity.sourcePtsTicks = info.sourcePtsTicks;
  identity.sourceDtsTicks = info.sourceDtsTicks;
  identity.ptsUs = ptsUs;
  identity.durationUs = durationUs;
  return identity;
}

inline bool sameIdentity(const FrameIdentity& left,
                         const FrameIdentity& right) {
  constexpr int64_t kUnknown = (std::numeric_limits<int64_t>::min)();
  if (left.sourcePtsTicks != kUnknown && right.sourcePtsTicks != kUnknown) {
    if (left.sourcePtsTicks != right.sourcePtsTicks) {
      return false;
    }
    if (left.sourceDtsTicks != kUnknown && right.sourceDtsTicks != kUnknown) {
      return left.sourceDtsTicks == right.sourceDtsTicks;
    }
    return true;
  }
  if (left.sourceDtsTicks != kUnknown && right.sourceDtsTicks != kUnknown) {
    return left.sourceDtsTicks == right.sourceDtsTicks;
  }
  return left.ptsUs == right.ptsUs && left.durationUs == right.durationUs;
}

struct SourceFrame {
  VideoFrame frame;
  VideoReadInfo info{};
  FrameIdentity identity;
  int64_t sourcePtsUs = 0;
  int64_t durationUs = 0;
  double decodeMs = 0.0;
};

struct CoverageSpan {
  int64_t startUs = 0;
  int64_t endUs = 0;

  bool valid() const { return startUs >= 0 && endUs > startUs; }
  bool covers(int64_t start, int64_t end) const {
    return valid() && startUs <= start && endUs >= end;
  }
};

class SourceFrameCache {
 public:
  bool insert(std::shared_ptr<const SourceFrame> frame);
  bool insertBatch(
      const std::vector<std::shared_ptr<const SourceFrame>>& frames);
  std::shared_ptr<const SourceFrame> find(
      const FrameIdentity& identity) const;
  std::vector<std::shared_ptr<const SourceFrame>> framesInRange(
      int64_t startUs, int64_t endUs) const;

  void addCoverage(int64_t startUs, int64_t endUs);
  bool covers(int64_t startUs, int64_t endUs) const;
  std::optional<CoverageSpan> bestCoverageFor(int64_t startUs,
                                               int64_t endUs) const;
  void retainRange(int64_t startUs, int64_t endUs);
  void clear();

  const std::shared_ptr<const SourceFrame>& first() const;
  const std::shared_ptr<const SourceFrame>& last() const;
  size_t size() const { return frames_.size(); }
  bool empty() const { return frames_.empty(); }

 private:
  std::vector<std::shared_ptr<const SourceFrame>> frames_;
  std::vector<CoverageSpan> coverage_;
};

enum class DecodePlanKind {
  CacheHit,
  ContinueForward,
  SeekForward,
};

struct DecodePlan {
  DecodePlanKind kind = DecodePlanKind::SeekForward;
  int64_t decodeStartUs = 0;
  int64_t decodeEndUs = 0;
  std::optional<FrameIdentity> stopAtIdentity;

  bool valid() const {
    return kind == DecodePlanKind::CacheHit ||
           (decodeStartUs >= 0 && decodeEndUs > decodeStartUs);
  }
};

DecodePlan planDecode(const SourceFrameCache& cache, int64_t rangeStartUs,
                      int64_t rangeEndUs,
                      const std::optional<FrameIdentity>& decoderTail);

}  // namespace playback_video_frame_step_prefetch
