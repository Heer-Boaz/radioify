#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

#include <d3d11.h>

#include "playback/video/decoder.h"
#include "playback/video/frame_step.h"

namespace playback_video_frame_step_prefetch {

// Keep one decoded second as two half-second segments: the cursor owns the
// active segment while the worker prepares the next segment. Swapping an
// already-decoded segment near the cursor edge keeps reverse seek/decode work
// off the presentation path. Independent frame and one-GiB logical-byte
// ceilings prevent malformed timing or very large surfaces from turning the
// cache into unbounded storage.
inline constexpr int64_t kWindowDurationUs = 1000000;
inline constexpr int64_t kSegmentDurationUs = kWindowDurationUs / 2;
inline constexpr size_t kMaxCachedFrameCount = 240;
inline constexpr size_t kMaxCachedBytes =
    size_t{1} * 1024u * 1024u * 1024u;

enum class RequestKind {
  Around,
  Before,
  After,
};

enum class BatchSide {
  Before,
  After,
};

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

struct Boundary {
  FrameIdentity identity;
  int64_t ptsUs = 0;
  int64_t durationUs = 0;
  int64_t sourcePtsUs = (std::numeric_limits<int64_t>::min)();

  bool valid() const { return ptsUs >= 0 && durationUs > 0; }
};

struct CachedFrame {
  VideoFrame frame;
  VideoReadInfo info{};
  int64_t ptsUs = 0;
  int64_t durationUs = 0;
  double decodeMs = 0.0;
};

struct Batch {
  int serial = 0;
  uint64_t generation = 0;
  RequestKind requestKind = RequestKind::Around;
  BatchSide side = BatchSide::Before;
  Boundary boundary;
  std::vector<CachedFrame> frames;
};

struct Request {
  int serial = 0;
  RequestKind kind = RequestKind::Around;
  Boundary boundary;
  int64_t rangeStartUs = 0;
  int64_t rangeEndUs = 0;
};

class Prefetcher {
 public:
  Prefetcher();
  ~Prefetcher();

  Prefetcher(const Prefetcher&) = delete;
  Prefetcher& operator=(const Prefetcher&) = delete;

  bool start(const std::filesystem::path& path, int videoStreamIndex,
             ID3D11Device* device, std::recursive_mutex* contextMutex);
  void stop();
  void invalidate();

  bool request(const Request& request);
  bool busyFor(int serial,
               playback_video_frame_step::Direction direction) const;
  std::vector<Batch> takeBatches(int serial);
  bool takeFailure(int serial);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_frame_step_prefetch
