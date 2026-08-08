#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

#include "playback/video/timeline_preview_types.h"

namespace playback_video_timeline_preview {

// UI-thread model for the seek-bar hover contract.  It owns presentation
// state and timestamp quantization; the thumbnail provider never sees cursor
// coordinates, progress-bar geometry, or visibility state.
class HoverModel {
 public:
  struct Update {
    bool changed = false;
    std::optional<Request> request;
  };

  void start(int64_t durationUs);
  void stop();

  Update hover(double ratio, int progressUnits);
  bool hide();
  bool apply(const Result& result);

  uint64_t requestId() const;
  Snapshot snapshot() const;

 private:
  uint64_t nextRequestIdLocked();

  mutable std::mutex mutex_;
  Snapshot snapshot_;
  uint64_t requestId_ = 0;
  int64_t activeDecodeTargetUs_ = -1;
  int64_t activeBucketUs_ = 0;
  int64_t lastRequestedDecodeTargetUs_ = -1;
};

}  // namespace playback_video_timeline_preview
