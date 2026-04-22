#include "video_luminance_stats.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kLumaByteMax = 255;

int clampLumaBin(std::size_t value) {
  return static_cast<int>(std::min<std::size_t>(value, kLumaByteMax));
}

int findPercentileBin(const uint32_t* histogram, std::size_t histogramSize,
                      uint64_t target) {
  uint64_t accum = 0;
  for (std::size_t i = 0; i < histogramSize; ++i) {
    accum += histogram[i];
    if (accum >= target) {
      return clampLumaBin(i);
    }
  }
  return clampLumaBin(histogramSize - 1);
}

int smoothLumaBoundary(int current, int previous,
                       const VideoLuminancePercentileConfig& config) {
  const int delta = current - previous;
  if (std::abs(delta) >= config.resetDelta) {
    return current;
  }
  return previous + ((delta * config.smoothAlphaQ8) >> 8);
}
}  // namespace

void ResetVideoLuminanceHistory(VideoLuminanceHistory& history) {
  history.low = -1;
  history.high = -1;
}

VideoLuminanceStats ResolveVideoLuminanceStatsFromHistogram(
    const uint32_t* histogram, std::size_t histogramSize,
    uint64_t sampleCount, const VideoLuminancePercentileConfig& config,
    VideoLuminanceHistory* history) {
  VideoLuminanceStats stats;
  stats.sampleCount = sampleCount;

  if (sampleCount > 0 && histogramSize > 0) {
    const uint64_t lowTarget = static_cast<uint64_t>(std::max(
        1.0, std::round(static_cast<double>(sampleCount) *
                        config.lowPercentile)));
    const uint64_t highTarget = static_cast<uint64_t>(std::max(
        1.0, std::round(static_cast<double>(sampleCount) *
                        config.highPercentile)));
    stats.low = findPercentileBin(histogram, histogramSize, lowTarget);
    stats.high = findPercentileBin(histogram, histogramSize, highTarget);
    if (stats.high <= stats.low) {
      stats.high = std::min(kLumaByteMax, stats.low + 1);
    }
  }

  if (history && history->low >= 0 && history->high >= 0) {
    stats.low = smoothLumaBoundary(stats.low, history->low, config);
    stats.high = smoothLumaBoundary(stats.high, history->high, config);
  }
  if (stats.high <= stats.low) {
    stats.high = std::min(kLumaByteMax, stats.low + 1);
  }

  if (history) {
    history->low = stats.low;
    history->high = stats.high;
  }

  stats.range = std::max(config.minimumRange, stats.high - stats.low);
  return stats;
}
