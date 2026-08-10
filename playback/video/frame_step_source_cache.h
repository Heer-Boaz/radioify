#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "playback/video/decoder.h"
#include "playback/video/frame_step.h"

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
  int64_t ptsUs = 0;
  int64_t sourcePtsUs = 0;
  int64_t durationUs = 0;
  int64_t sourceDurationUs = 0;
  double decodeMs = 0.0;
};

struct FrameWindow {
  std::vector<std::shared_ptr<const SourceFrame>> frames;
  size_t anchorIndex = 0;

  bool valid() const { return !frames.empty() && anchorIndex < frames.size(); }
};

// The output thread is the sole owner of this cache. Decoder workers publish
// immutable, forward-decoded runs; a run is committed only when its exact join
// identity agrees with the existing chain. Consequently adjacency, rather than
// a separately maintained timestamp interval, is the cache's source of truth.
class SourceFrameCache {
 public:
  bool commitDecodedRun(
      playback_video_frame_step::Direction direction,
      const FrameIdentity& joinIdentity,
      std::shared_ptr<const SourceFrame> joinFrame,
      const std::vector<std::shared_ptr<const SourceFrame>>& decodedFrames);

  std::shared_ptr<const SourceFrame> find(
      const FrameIdentity& identity) const;
  FrameWindow windowAround(const FrameIdentity& anchorIdentity,
                           int64_t beforeDurationUs, int64_t afterDurationUs,
                           size_t maximumFrameCount) const;
  bool retainWindow(const FrameIdentity& anchorIdentity,
                    int64_t beforeDurationUs, int64_t afterDurationUs,
                    size_t maximumFrameCount);
  void clear();

  size_t size() const { return nodes_.size(); }
  bool empty() const { return nodes_.empty(); }

 private:
  struct Node {
    std::shared_ptr<const SourceFrame> frame;
    std::optional<FrameIdentity> previous;
    std::optional<FrameIdentity> next;
  };

  std::vector<Node> nodes_;
};

}  // namespace playback_video_frame_step_prefetch
