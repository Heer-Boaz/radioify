#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct VideoFrameSnapshot {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t strideBytes = 0;
  std::vector<uint8_t> rgba;
};

struct VideoFrameSnapshotResult {
  VideoFrameSnapshot snapshot;
  std::string error;

  bool succeeded() const {
    return error.empty() && snapshot.width > 0 && snapshot.height > 0 &&
           !snapshot.rgba.empty();
  }
};
