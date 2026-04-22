#pragma once

#include <cstddef>
#include <cstdint>

struct VideoLuminancePercentileConfig {
  double lowPercentile;
  double highPercentile;
  int minimumRange;
  int smoothAlphaQ8;
  int resetDelta;
};

struct VideoLuminanceHistory {
  int low = -1;
  int high = -1;
};

struct VideoLuminanceStats {
  int low = 0;
  int high = 255;
  int range = 255;
  uint64_t sampleCount = 0;
};

void ResetVideoLuminanceHistory(VideoLuminanceHistory& history);

VideoLuminanceStats ResolveVideoLuminanceStatsFromHistogram(
    const uint32_t* histogram, std::size_t histogramSize,
    uint64_t sampleCount, const VideoLuminancePercentileConfig& config,
    VideoLuminanceHistory* history);
