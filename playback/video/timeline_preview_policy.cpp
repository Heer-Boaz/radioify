#include "playback/video/timeline_preview_types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace playback_video_timeline_preview {
namespace {

constexpr int64_t kMinimumBucketUs = 250000;
constexpr int64_t kMaximumBucketUs = 10000000;
constexpr int kMinimumProgressUnits = 24;
constexpr int kMaximumProgressUnits = 512;

int makeEvenAtLeastTwo(int value) {
  value = std::max(2, value);
  return value & 1 ? value + 1 : value;
}

}  // namespace

int64_t bucketDurationUs(int64_t durationUs, int progressUnits) {
  if (durationUs <= 0) return kMinimumBucketUs;
  const int units =
      std::clamp(progressUnits, kMinimumProgressUnits, kMaximumProgressUnits);
  const int64_t perUnit = std::max<int64_t>(
      1, durationUs / units + (durationUs % units != 0 ? 1 : 0));
  return std::clamp(perUnit, kMinimumBucketUs, kMaximumBucketUs);
}

int64_t bucketTargetUs(int64_t targetUs, int64_t durationUs,
                       int64_t bucketUs) {
  if (durationUs <= 0) return 0;
  const int64_t lastUs = std::max<int64_t>(0, durationUs - 1);
  targetUs = std::clamp(targetUs, int64_t{0}, lastUs);
  if (targetUs == 0 || targetUs == lastUs) return targetUs;
  bucketUs = std::max<int64_t>(1, bucketUs);
  const int64_t bucketStart = (targetUs / bucketUs) * bucketUs;
  const int64_t half = bucketUs / 2;
  if (bucketStart > lastUs - std::min(lastUs, half)) return lastUs;
  return std::min(lastUs, bucketStart + half);
}

std::pair<int, int> fitDecodeSize(int sourceWidth, int sourceHeight,
                                  int maxWidth, int maxHeight) {
  sourceWidth = std::max(2, sourceWidth);
  sourceHeight = std::max(2, sourceHeight);
  maxWidth = makeEvenAtLeastTwo(maxWidth);
  maxHeight = makeEvenAtLeastTwo(maxHeight);
  const double scale =
      std::min(static_cast<double>(maxWidth) / sourceWidth,
               static_cast<double>(maxHeight) / sourceHeight);
  int width = makeEvenAtLeastTwo(
      static_cast<int>(std::floor(sourceWidth * std::min(1.0, scale))));
  int height = makeEvenAtLeastTwo(
      static_cast<int>(std::floor(sourceHeight * std::min(1.0, scale))));
  width = std::min(width, maxWidth);
  height = std::min(height, maxHeight);
  return {width, height};
}

std::string formatTimestamp(int64_t timestampUs) {
  timestampUs = std::max<int64_t>(0, timestampUs);
  const int64_t totalSeconds =
      timestampUs / 1000000 +
      (timestampUs % 1000000 >= 500000 ? int64_t{1} : int64_t{0});
  const int64_t hours = totalSeconds / 3600;
  const int64_t minutes = (totalSeconds % 3600) / 60;
  const int64_t seconds = totalSeconds % 60;
  char buffer[32]{};
  if (hours > 0) {
    std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld",
                  static_cast<long long>(hours),
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%lld:%02lld",
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds));
  }
  return buffer;
}

std::vector<int64_t> prefetchTargets(int64_t targetUs, int64_t bucketUs,
                                     int64_t durationUs, int direction) {
  std::vector<int64_t> targets;
  if (durationUs <= 0) return targets;
  const int64_t lastUs = durationUs - 1;
  targetUs = std::clamp(targetUs, int64_t{0}, lastUs);
  bucketUs = std::max<int64_t>(1, bucketUs);

  const auto offsetTarget = [&](int sign, int distance) {
    const int safeDistance = std::max(1, distance);
    const int64_t distanceUs =
        bucketUs > (std::numeric_limits<int64_t>::max)() / safeDistance
            ? (std::numeric_limits<int64_t>::max)()
            : bucketUs * safeDistance;
    if (sign < 0) {
      return targetUs < distanceUs ? int64_t{0} : targetUs - distanceUs;
    }
    return targetUs > lastUs - std::min(lastUs, distanceUs)
               ? lastUs
               : targetUs + distanceUs;
  };
  const auto append = [&](int sign, int distance) {
    const int64_t candidate = offsetTarget(sign, distance);
    if (candidate != targetUs &&
        std::find(targets.begin(), targets.end(), candidate) == targets.end()) {
      targets.push_back(candidate);
    }
  };

  if (direction != 0) {
    append(direction, 1);
    append(direction, 2);
    append(-direction, 1);
  } else {
    append(1, 1);
    append(-1, 1);
  }
  return targets;
}

CellLayout layoutCells(int columns, int rows, int progressBarY,
                       int progressBarX, int progressBarWidth,
                       double anchorRatio, int sourceWidth, int sourceHeight,
                       double cellPixelWidth, double cellPixelHeight,
                       const std::string& label,
                       const std::vector<std::string>& metadataLines) {
  CellLayout out;
  if (columns < 10 || rows < 6) return out;

  if (progressBarY <= 0 || progressBarY >= rows) return out;
  const int availableRows = progressBarY;
  if (availableRows < 4) return out;

  const double safeCellWidth = cellPixelWidth > 0.0 ? cellPixelWidth : 9.0;
  const double safeCellHeight = cellPixelHeight > 0.0 ? cellPixelHeight : 21.0;
  const double sourceAspect =
      sourceWidth > 0 && sourceHeight > 0
          ? static_cast<double>(sourceWidth) / sourceHeight
          : 16.0 / 9.0;

  const bool placeMetadataBeside =
      !metadataLines.empty() && columns >= 72 && availableRows >= 7;
  const int reservedMetadataColumns =
      placeMetadataBeside ? std::clamp(columns / 3, 24, 38) + 1 : 0;
  const int requestedMetadataRows = metadataLines.empty()
                                        ? 0
                                        : std::min({4,
                                                    static_cast<int>(
                                                        metadataLines.size()),
                                                    std::max(
                                                        0,
                                                        availableRows - 4)});
  const int reservedMetadataRows =
      !metadataLines.empty() && !placeMetadataBeside
          ? requestedMetadataRows
          : 0;
  const int maxImageColumns = std::max(
      6, std::min({40, columns - 2 - reservedMetadataColumns,
                   std::max(12, columns / 3)}));
  const int maxImageRows = std::max(
      2, std::min(12, availableRows - 2 - reservedMetadataRows));
  int imageColumns = maxImageColumns;
  int imageRows = std::max(
      2, static_cast<int>(std::lround(
             imageColumns * safeCellWidth / (sourceAspect * safeCellHeight))));
  if (imageRows > maxImageRows) {
    imageRows = maxImageRows;
    imageColumns = std::max(
        6, static_cast<int>(std::lround(
               imageRows * sourceAspect * safeCellHeight / safeCellWidth)));
    imageColumns = std::min(imageColumns, maxImageColumns);
  }

  if (placeMetadataBeside) {
    out.metadataPlacement = CellLayout::MetadataPlacement::BesideImage;
    out.metadataWidth = reservedMetadataColumns - 1;
    out.metadataHeight = std::min(requestedMetadataRows, imageRows);
    out.outerWidth = imageColumns + 1 + out.metadataWidth + 2;
    out.outerHeight = std::max(imageRows, out.metadataHeight) + 2;
  } else if (!metadataLines.empty()) {
    out.metadataPlacement = CellLayout::MetadataPlacement::BelowImage;
    out.metadataWidth = imageColumns;
    out.metadataHeight = requestedMetadataRows;
    out.outerWidth = imageColumns + 2;
    out.outerHeight = imageRows + out.metadataHeight + 2;
  } else {
    out.outerWidth = imageColumns + 2;
    out.outerHeight = imageRows + 2;
  }
  if (out.outerWidth > columns || out.outerHeight > availableRows) {
    return CellLayout{};
  }

  const int barX = progressBarX >= 0 ? progressBarX : 1;
  const int barWidth =
      progressBarWidth > 0 ? progressBarWidth : std::max(1, columns - 2);
  const double ratio = std::clamp(anchorRatio, 0.0, 1.0);
  const int anchorX = barX + static_cast<int>(
                                std::lround(ratio * std::max(0, barWidth - 1)));
  out.outerX =
      std::clamp(anchorX - out.outerWidth / 2, 0, columns - out.outerWidth);
  // The preview is one seek-bar-owned popover: image first, then its time
  // footer, immediately followed by the progress bar.  Keeping the footer at
  // a fixed Y also prevents it from jumping when the async image arrives.
  out.outerY = progressBarY - out.outerHeight;
  out.imageX = out.outerX + 1;
  out.imageY = out.outerY + 1;
  out.imageWidth = imageColumns;
  out.imageHeight = imageRows;
  if (out.metadataPlacement ==
      CellLayout::MetadataPlacement::BesideImage) {
    out.metadataX = out.imageX + out.imageWidth + 1;
    out.metadataY = out.imageY;
  } else if (out.metadataPlacement ==
             CellLayout::MetadataPlacement::BelowImage) {
    out.metadataX = out.imageX;
    out.metadataY = out.imageY + out.imageHeight;
  }
  out.metadataLines.assign(
      metadataLines.begin(),
      metadataLines.begin() +
          std::min<std::size_t>(metadataLines.size(),
                                static_cast<std::size_t>(
                                    out.metadataHeight)));
  out.label = label;
  const int labelWidth = static_cast<int>(out.label.size());
  out.labelX = out.outerX + std::max(1, (out.outerWidth - labelWidth) / 2);
  out.labelY = progressBarY - 1;
  return out;
}

}  // namespace playback_video_timeline_preview
