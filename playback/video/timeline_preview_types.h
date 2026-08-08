#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "playback/video/decoder.h"

namespace playback_video_timeline_preview {

// Hover previews intentionally use a bounded, sparse storyboard cache.  It is
// separate from the contiguous frame-step cache because the two caches have
// opposite locality and eviction requirements.
inline constexpr size_t kMaxCacheBytes = 32u * 1024u * 1024u;
inline constexpr size_t kMaxCacheFrames = 256u;
inline constexpr int kDecodeMaxWidth = 320;
inline constexpr int kDecodeMaxHeight = 180;

struct Image {
  uint64_t id = 0;
  int64_t requestedUs = 0;
  int64_t frameUs = 0;
  VideoFrame frame;
};

struct Snapshot {
  bool visible = false;
  bool loading = false;
  bool failed = false;
  double anchorRatio = 0.0;
  int64_t targetUs = 0;
  int64_t durationUs = 0;
  uint64_t revision = 0;
  std::shared_ptr<const Image> image;
};

struct CellLayout {
  int outerX = 0;
  int outerY = 0;
  int outerWidth = 0;
  int outerHeight = 0;
  int imageX = 0;
  int imageY = 0;
  int imageWidth = 0;
  int imageHeight = 0;
  int labelX = 0;
  int labelY = 0;
  std::string label;

  bool drawable() const {
    return outerWidth >= 4 && outerHeight >= 3 && imageWidth > 0 &&
           imageHeight > 0;
  }
};

int64_t bucketDurationUs(int64_t durationUs, int progressUnits);
int64_t bucketTargetUs(int64_t targetUs, int64_t durationUs,
                       int64_t bucketUs);
std::pair<int, int> fitDecodeSize(int sourceWidth, int sourceHeight,
                                  int maxWidth = kDecodeMaxWidth,
                                  int maxHeight = kDecodeMaxHeight);
std::string formatTimestamp(int64_t timestampUs);

CellLayout layoutCells(int columns, int rows, int footerTopRow,
                       int progressBarX, int progressBarWidth,
                       double anchorRatio, int sourceWidth, int sourceHeight,
                       double cellPixelWidth, double cellPixelHeight,
                       const std::string& label);

}  // namespace playback_video_timeline_preview
