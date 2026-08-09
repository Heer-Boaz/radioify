#pragma once

#include <cstdint>
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

  void start(int64_t durationUs, int sourceWidth = 0, int sourceHeight = 0);
  void stop();

  Update hover(PresentationSurface surface, double ratio, int progressUnits);
  bool hide(PresentationSurface surface);
  bool reject(const Request& request);
  bool apply(const Result& result);

  uint64_t requestId() const;
  Snapshot snapshot() const;
  Snapshot snapshotFor(PresentationSurface surface) const;

 private:
  uint64_t nextRequestId();

  Snapshot snapshot_;
  uint64_t requestId_ = 0;
  int64_t activeDecodeTargetUs_ = -1;
  int64_t activeBucketUs_ = 0;
  int64_t lastRequestedDecodeTargetUs_ = -1;
};

}  // namespace playback_video_timeline_preview
