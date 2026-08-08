#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "playback/video/timeline_preview_types.h"

namespace playback_video_timeline_preview {

enum class DecodeStatus : uint8_t {
  Ready,
  Cancelled,
  Failed,
};

struct DecodeResult {
  DecodeStatus status = DecodeStatus::Failed;
  int64_t frameUs = 0;
  VideoFrame frame;
};

// Worker-owned precise-seek decoder.  It has no cache, hover, scheduling, or
// presentation responsibilities.
class Decoder {
 public:
  Decoder();
  ~Decoder();

  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  bool configure(const Source& source, std::atomic<bool>* stopping,
                 std::atomic<uint64_t>* latestRequestId);
  DecodeResult decode(int64_t targetUs, uint64_t requestId);
  void reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_timeline_preview
