#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "playback/video/image.h"

namespace playback_video_timeline_preview {

// Hover previews intentionally use a bounded, sparse exact-frame cache. It is
// separate from the contiguous frame-step cache because the two caches have
// opposite locality and eviction requirements.
inline constexpr size_t kMaxCacheBytes = 32u * 1024u * 1024u;
inline constexpr size_t kMaxCacheFrames = 256u;
inline constexpr int kDecodeMaxWidth = 320;
inline constexpr int kDecodeMaxHeight = 180;

struct Image {
  uint64_t id = 0;
  int64_t requestedUs = 0;
  std::optional<int64_t> decodedFrameUs;
  playback_video_image::RgbaImage surface;
};

struct Source {
  std::filesystem::path path;
  int videoStreamIndex = -1;
  int64_t durationUs = 0;
  int sourceWidth = 0;
  int sourceHeight = 0;
};

struct Request {
  uint64_t id = 0;
  int64_t targetUs = 0;
  std::vector<int64_t> prefetchTargetsUs;
};

enum class ResultOrigin : uint8_t {
  Decoded,
  MemoryCache,
  PersistentCache,
};

struct Result {
  uint64_t requestId = 0;
  int64_t targetUs = 0;
  ResultOrigin origin = ResultOrigin::Decoded;
  std::shared_ptr<const Image> image;
};

enum class PresentationSurface : uint8_t {
  Terminal,
  VideoWindow,
};

enum class MetadataRole : uint8_t { Title, Detail };

struct MetadataLine {
  std::string text;
  MetadataRole role = MetadataRole::Detail;
};

struct Snapshot {
  // Hover intent and image readiness are deliberately independent. Renderers
  // may present the timestamp while the provider is working, but must never
  // infer a drawable thumbnail from hover activity alone.
  bool hoverActive = false;
  PresentationSurface presentationSurface = PresentationSurface::Terminal;
  double anchorRatio = 0.0;
  // Exact edited/program position shown to the user.
  int64_t targetUs = 0;
  // Exact source position used for source-coordinate semantic metadata. This
  // is deliberately independent of the bucketed frame decode request.
  int64_t sourceTargetUs = 0;
  int64_t durationUs = 0;
  int sourceWidth = 0;
  int sourceHeight = 0;
  uint64_t revision = 0;
  std::shared_ptr<const Image> image;
  // Session-owned semantic context for the hovered timeline position. The
  // preview provider owns only images; text is projected by
  // the session so image decoding never depends on analysis lifecycle.
  std::vector<MetadataLine> metadataLines;

  bool hasImage() const {
    return image && playback_video_image::validate(image->surface);
  }
};

struct CellLayout {
  enum class MetadataPlacement : uint8_t {
    None,
    BesideImage,
    BelowImage,
  };

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
  MetadataPlacement metadataPlacement = MetadataPlacement::None;
  int metadataX = 0;
  int metadataY = 0;
  int metadataWidth = 0;
  int metadataHeight = 0;
  std::vector<MetadataLine> metadataLines;

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
std::vector<int64_t> prefetchTargets(int64_t targetUs, int64_t bucketUs,
                                     int64_t durationUs, int direction);

CellLayout layoutCells(int columns, int rows, int progressBarY,
                       int progressBarX, int progressBarWidth,
                       double anchorRatio, int sourceWidth, int sourceHeight,
                       double cellPixelWidth, double cellPixelHeight,
                       const std::string& label,
                       const std::vector<MetadataLine>& metadataLines = {});

}  // namespace playback_video_timeline_preview
