#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <d3d11.h>

#include "playback/video/decoder.h"
#include "playback/video/frame_step.h"
#include "playback/video/frame_step_source_cache.h"

namespace playback_video_frame_step_prefetch {

// Keep one direction-neutral decoded source cache with a one-second active
// prefetch horizon plus exact inverse neighbors as a guard. The source cache
// fills only uncovered ranges. Independent frame and one-GiB logical-byte
// ceilings prevent malformed timing or very large surfaces from becoming
// unbounded.
inline constexpr int64_t kWindowDurationUs = 1000000;
inline constexpr int64_t kRefillLeadDurationUs = 250000;
inline constexpr size_t kRefillLeadFrameCount = 8;
inline constexpr size_t kDirectionChangeReserveFrameCount = 5;
inline constexpr size_t kMaxCachedFrameCount = 240;
inline constexpr size_t kMaxCachedBytes =
    size_t{1} * 1024u * 1024u * 1024u;

inline bool refillNeeded(size_t frameCount, int64_t durationUs) {
  return frameCount <= kRefillLeadFrameCount ||
         durationUs <= kRefillLeadDurationUs;
}

struct Boundary {
  FrameIdentity identity;
  int64_t ptsUs = 0;
  int64_t durationUs = 0;
  int64_t sourcePtsUs = (std::numeric_limits<int64_t>::min)();

  bool valid() const { return ptsUs >= 0 && durationUs > 0; }
};

struct FrameView {
  std::shared_ptr<const SourceFrame> source;
  int64_t ptsUs = 0;

  bool valid() const { return source != nullptr; }
};

struct Result {
  int serial = 0;
  uint64_t generation = 0;
  Boundary boundary;
  int64_t rangeStartUs = 0;
  int64_t rangeEndUs = 0;
  bool decoderSeeked = false;
  bool cacheHit = false;
  size_t decodedFrameCount = 0;
  std::vector<FrameView> frames;
};

struct Request {
  int serial = 0;
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
  std::vector<Result> takeResults(int serial);
  std::optional<Request> takeFailure(int serial);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_frame_step_prefetch
